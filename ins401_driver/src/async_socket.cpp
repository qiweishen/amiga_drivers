#include "async_socket.h"



bool AsyncPacketSocket::Open() {
	// Create raw socket with packet mmap
	raw_fd_ = socket(AF_PACKET, SOCK_RAW, htons(config_.protocol));
	if (raw_fd_ < 0) {
		const std::error_code ec(errno, std::generic_category());
		LogMsg(fmt::format("Failed to create socket: {}", ec.message()), spdlog::level::err);
		return false;
	}

	// Set socket options for high performance
	int val = 1;
	if (setsockopt(raw_fd_, SOL_PACKET, PACKET_LOSS, &val, sizeof(val)) < 0) {
		const std::error_code ec(errno, std::generic_category());
		LogMsg("Failed to set PACKET_LOSS option", spdlog::level::warn);
	}

	// Set receive buffer size
	int rcvbuf = 32 * 1024 * 1024;	// 32MB
	if (setsockopt(raw_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
		LogMsg("Failed to set SO_RCVBUF option", spdlog::level::warn);
	}

	// Set send buffer size
	int sndbuf = 8 * 1024 * 1024;  // 8MB
	if (setsockopt(raw_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
		LogMsg("Failed to set SO_SNDBUF option", spdlog::level::warn);
	}

	// Set TPACKET_V3
	val = TPACKET_V3;
	if (setsockopt(raw_fd_, SOL_PACKET, PACKET_VERSION, &val, sizeof(val)) < 0) {
		LogMsg("Failed to set TPACKET_V3, use the default TPACKET_V1", spdlog::level::warn);
	}

	SocketGuard fd_guard(raw_fd_);

	// Setup RX ring / TX ring / Bind interface
	if (!SetupRXRing() || !SetupTXRing() || !BindInterface()) {
		return false;
	}

	// Setup fanout if requested
	if (config_.fanout_group >= 0) {
		SetupFanout();
	}

	// Set promiscuous mode if requested
	if (config_.promiscuous) {
		SetPromiscuousMode();
	}

	// Assign to boost::asio
	socket_descriptor_.assign(raw_fd_);
	fd_guard.release();
	running_ = true;

	LogMsg(fmt::format("Successfully opened socket on interface {}", config_.interface_name), spdlog::level::trace);

	return true;
}


void AsyncPacketSocket::Close() noexcept {
	running_ = false;

	if (socket_descriptor_.is_open()) {
		boost::system::error_code ec;
		socket_descriptor_.close(ec);
		if (ec) {
			LogMsg("Failed to close socket_descriptor_", spdlog::level::warn);
		}
		raw_fd_ = -1;
	} else if (raw_fd_ >= 0) {
		::close(raw_fd_);
		raw_fd_ = -1;
	}

	// Unmap RX ring and TX ring
	rx_ring_.reset();
	tx_ring_.reset();
}


void AsyncPacketSocket::AsyncReceive(ReceiveHandler &&handler) {
	if (!running_.load(std::memory_order_acquire)) {
		boost::asio::post(io_context_, [handler]() { handler(nullptr, 0, boost::asio::error::operation_aborted); });
		return;
	}

	auto self = shared_from_this();
	socket_descriptor_.async_wait(boost::asio::posix::descriptor::wait_read,
								  [self, handler = std::forward<ReceiveHandler>(handler)](const boost::system::error_code &ec) {
									  if (ec) {
										  handler(nullptr, 0, ec);
										  return;
									  }

									  // Process received packets from ring buffer
									  self->ProcessRXRing(handler);
								  });
}


void AsyncPacketSocket::AsyncSend(const uint8_t *data, size_t length, SendHandler &&handler) {
	if (!running_.load(std::memory_order_acquire)) {
		boost::asio::post(io_context_, [handler]() { handler(boost::asio::error::operation_aborted, 0); });
		return;
	}

	auto data_copy = std::make_shared<std::vector<uint8_t>>(data, data + length);
	auto self = shared_from_this();
	boost::asio::post(self->io_context_, [self, data_copy, handler = std::move(handler)] {
		self->SendTXRing(data_copy->data(), data_copy->size(), handler);
	});
}


tpacket_stats_v3 AsyncPacketSocket::GetKernelStats() const noexcept {
	tpacket_stats_v3 stats{};
	if (raw_fd_ < 0) {
		return stats;
	}
	socklen_t len = sizeof(stats);
	if (::getsockopt(raw_fd_, SOL_PACKET, PACKET_STATISTICS, &stats, &len) < 0) {
		LogMsg("Failed to get kernel statistics", spdlog::level::warn);
		return stats;
	}
	return stats;
}


bool AsyncPacketSocket::SetupRXRing() {
	rx_ring_.reset();
	// Setup RX ring
	rx_ring_.req = tpacket_req3{};
	rx_ring_.req.tp_block_size = config_.block_size;
	rx_ring_.req.tp_frame_size = config_.frame_size;
	rx_ring_.req.tp_block_nr = config_.block_nr;
	rx_ring_.req.tp_frame_nr = config_.block_size * config_.block_nr / config_.frame_size;
	rx_ring_.req.tp_retire_blk_tov = config_.retire_blk_tov;
	rx_ring_.req.tp_feature_req_word = TP_FT_REQ_FILL_RXHASH;

	if (setsockopt(raw_fd_, SOL_PACKET, PACKET_RX_RING, &rx_ring_.req, sizeof(rx_ring_.req)) < 0) {
		LogMsg("Failed to setup RX ring", spdlog::level::err);
		return false;
	}

	// Map RX ring buffer
	rx_ring_.map_size = rx_ring_.req.tp_block_size * rx_ring_.req.tp_block_nr;
	auto *ptr = static_cast<uint8_t *>(
			mmap(nullptr, rx_ring_.map_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED | MAP_POPULATE, raw_fd_, 0));

	if (rx_ring_.map == MAP_FAILED) {
		LogMsg("Failed to map RX ring to memory", spdlog::level::err);
		rx_ring_.reset();
		return false;
	}
	rx_ring_.map = ptr;

	// Setup iovec for RX blocks
	rx_ring_.rd.resize(rx_ring_.req.tp_block_nr);
	for (unsigned i = 0; i < rx_ring_.req.tp_block_nr; ++i) {
		rx_ring_.rd[i].iov_base = rx_ring_.map + (i * rx_ring_.req.tp_block_size);
		rx_ring_.rd[i].iov_len = rx_ring_.req.tp_block_size;
	}

	return true;
}


bool AsyncPacketSocket::SetupTXRing() {
	tx_ring_.reset();
	// Setup TX ring
	tx_ring_.req = tpacket_req3{};
	tx_ring_.req.tp_block_size = config_.block_size;
	tx_ring_.req.tp_frame_size = config_.frame_size;
	tx_ring_.req.tp_block_nr = config_.block_nr;
	tx_ring_.req.tp_frame_nr = config_.block_size * config_.block_nr / config_.frame_size;

	if (setsockopt(raw_fd_, SOL_PACKET, PACKET_TX_RING, &tx_ring_.req, sizeof(tx_ring_.req)) < 0) {
		LogMsg("Failed to setup TX ring", spdlog::level::err);
		LogMsg(fmt::format("SetupTXRing on fd {} (iface {}), errno={}", raw_fd_, config_.interface_name, errno), spdlog::level::err);
		return false;
	}

	// Map TX ring buffer (note the offset for TX ring)
	tx_ring_.map_size = tx_ring_.req.tp_block_size * tx_ring_.req.tp_block_nr;
	const size_t rx_ring_size = rx_ring_.req.tp_block_size * rx_ring_.req.tp_block_nr;
	auto *ptr = static_cast<uint8_t *>(mmap(nullptr, tx_ring_.map_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED | MAP_POPULATE,
											raw_fd_, static_cast<off_t>(rx_ring_size)));

	if (tx_ring_.map == MAP_FAILED) {
		LogMsg("Failed to map TX ring to memory", spdlog::level::err);
		tx_ring_.reset();
		return false;
	}
	tx_ring_.map = ptr;

	// Setup iovec for TX blocks
	tx_ring_.rd.resize(tx_ring_.req.tp_block_nr);
	for (unsigned i = 0; i < tx_ring_.req.tp_block_nr; ++i) {
		tx_ring_.rd[i].iov_base = tx_ring_.map + (i * tx_ring_.req.tp_block_size);
		tx_ring_.rd[i].iov_len = tx_ring_.req.tp_block_size;
	}

	return true;
}


bool AsyncPacketSocket::BindInterface() {
	sockaddr_ll ll{};
	ll.sll_family = AF_PACKET;
	ll.sll_protocol = htons(config_.protocol);
	ll.sll_ifindex = static_cast<int>(if_nametoindex(config_.interface_name.c_str()));

	if (ll.sll_ifindex == 0) {
		LogMsg("Failed to get interface index", spdlog::level::err);
		return false;
	}

	if (bind(raw_fd_, reinterpret_cast<sockaddr *>(&ll), sizeof(ll)) < 0) {
		LogMsg("Failed to bind to interface", spdlog::level::err);
		return false;
	}

	return true;
}


void AsyncPacketSocket::SetupFanout() {
	int fanout_arg = (config_.fanout_group | (PACKET_FANOUT_HASH << 16));

	if (setsockopt(raw_fd_, SOL_PACKET, PACKET_FANOUT, &fanout_arg, sizeof(fanout_arg)) < 0) {
		LogMsg("Failed to set packet fanout", spdlog::level::warn);
	}
}


void AsyncPacketSocket::SetPromiscuousMode() {
	packet_mreq mreq{};
	mreq.mr_ifindex = static_cast<int>(if_nametoindex(config_.interface_name.c_str()));
	mreq.mr_type = PACKET_MR_PROMISC;

	if (setsockopt(raw_fd_, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		LogMsg("Failed to set promiscuous mode", spdlog::level::warn);
	}
}


void AsyncPacketSocket::ProcessRXRing(const ReceiveHandler &handler) {
	std::lock_guard<std::mutex> lock(rx_mutex_);

	tpacket_block_desc *pbd;
	tpacket3_hdr *ppd;

	pbd = static_cast<tpacket_block_desc *>(rx_ring_.rd[rx_ring_.current_block].iov_base);

	// Check if block is ready
	if ((pbd->hdr.bh1.block_status & TP_STATUS_USER) == 0) {
		// No packets available yet
		handler(nullptr, 0, boost::system::error_code());
		return;
	}

	// Process all packets in the block
	unsigned int num_pkts = pbd->hdr.bh1.num_pkts;
	ppd = reinterpret_cast<tpacket3_hdr *>(reinterpret_cast<uint8_t *>(pbd) + pbd->hdr.bh1.offset_to_first_pkt);

	for (unsigned int i = 0; i < num_pkts; ++i) {
		// Get packet data
		uint8_t *packet_data = reinterpret_cast<uint8_t *>(ppd) + ppd->tp_mac;
		size_t packet_len = ppd->tp_snaplen;

		// Update statistics
		++packets_received_;
		if (ppd->tp_status & TP_STATUS_LOSING) {
			++packets_dropped_;
		}

		// Call handler with packet data
		handler(packet_data, packet_len, boost::system::error_code());

		// Move to next packet
		ppd = reinterpret_cast<tpacket3_hdr *>(reinterpret_cast<uint8_t *>(ppd) + ppd->tp_next_offset);
	}

	// Release block back to kernel
	pbd->hdr.bh1.block_status = TP_STATUS_KERNEL;

	// Move to next block
	rx_ring_.current_block = (rx_ring_.current_block + 1) % rx_ring_.req.tp_block_nr;
}


void AsyncPacketSocket::SendTXRing(const uint8_t *data, size_t length, const SendHandler &handler) {
	std::lock_guard<std::mutex> lock(tx_mutex_);

	// Find available TX frame
	unsigned int frame_num = 0;
	tpacket3_hdr *header = nullptr;

	for (unsigned int i = 0; i < tx_ring_.req.tp_frame_nr; ++i) {
		frame_num = (tx_ring_.current_block + i) % tx_ring_.req.tp_frame_nr;
		const unsigned int block_num = frame_num / (tx_ring_.req.tp_block_size / tx_ring_.req.tp_frame_size);
		const unsigned int frame_offset =
				(frame_num % (tx_ring_.req.tp_block_size / tx_ring_.req.tp_frame_size)) * tx_ring_.req.tp_frame_size;

		header = reinterpret_cast<tpacket3_hdr *>(tx_ring_.map + (block_num * tx_ring_.req.tp_block_size) + frame_offset);

		if ((header->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING)) == 0) {
			// Found available frame
			break;
		}
		header = nullptr;
	}

	if (!header) {
		// No available TX frames
		handler(boost::asio::error::no_buffer_space, 0);
		return;
	}

	// Copy packet data to TX frame
	uint8_t *frame_data = reinterpret_cast<uint8_t *>(header) + TPACKET3_HDRLEN;
	size_t copy_len = std::min(length, tx_ring_.req.tp_frame_size - TPACKET3_HDRLEN);
	memcpy(frame_data, data, copy_len);

	// Set packet length and status
	header->tp_len = copy_len;
	header->tp_snaplen = copy_len;
	header->tp_status = TP_STATUS_SEND_REQUEST;

	// Trigger send
	if (send(raw_fd_, nullptr, 0, MSG_DONTWAIT) < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			handler(boost::system::error_code(errno, boost::system::generic_category()), 0);
			return;
		}
	}

	tx_ring_.current_block = (frame_num + 1) % tx_ring_.req.tp_frame_nr;
	++packets_sent_;

	handler(boost::system::error_code(), copy_len);
}


void AsyncPacketSocket::LogMsg(std::string_view msg, spdlog::level::level_enum log_level) const {
	if (log_level == spdlog::level::warn || log_level == spdlog::level::err || log_level == spdlog::level::critical) {
		const std::error_code ec(errno, std::generic_category());
		spdlog::log(log_level, "[AsyncPacketSocket] (Interface: {}) {}: {}", config_.interface_name, msg, ec.message());
	} else {
		spdlog::log(log_level, "[AsyncPacketSocket] (Interface: {}) {}", config_.interface_name, msg);
	}
}
