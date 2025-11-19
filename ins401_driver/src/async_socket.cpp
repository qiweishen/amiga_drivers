#include "async_socket.h"

#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <pcap/pcap.h>

#include "spdlog/spdlog.h"



AsyncRawSocket::AsyncRawSocket(boost::asio::io_context &io_context) : io_context_(io_context) {}


AsyncRawSocket::~AsyncRawSocket() {
	Close();
}


boost::system::error_code AsyncRawSocket::Open(const std::string &interface, const std::string &filter_expr) {
	interface_ = interface;

	// Create pcap handle
	char error_buffer[PCAP_ERRBUF_SIZE];
	pcap_handle_ = pcap_create(interface.c_str(), error_buffer);
	if (!pcap_handle_) {
		return LogMsg(error_buffer);
	}

	// Configure for high-frequency, low-latency capture
	pcap_set_snaplen(pcap_handle_, 65535);					  // Maximum packet size
	pcap_set_promisc(pcap_handle_, 1);						  // Promiscuous mode
	pcap_set_timeout(pcap_handle_, 0);						  // Non-blocking
	pcap_set_buffer_size(pcap_handle_, KERNEL_BUFFER_SIZE_);  // Large buffer
	pcap_set_immediate_mode(pcap_handle_, 1);				  // Immediate packet delivery

	// Activate the handle
	int status = pcap_activate(pcap_handle_);
	if (status < 0) {
		pcap_close(pcap_handle_);
		pcap_handle_ = nullptr;
		return LogMsg(pcap_statustostr(status));
	}

	// Set non-blocking mode
	if (pcap_setnonblock(pcap_handle_, 1, error_buffer) < 0) {
		pcap_close(pcap_handle_);
		pcap_handle_ = nullptr;
		return LogMsg(error_buffer);
	}

	// Get file descriptor for async operations
	pcap_fd_ = pcap_get_selectable_fd(pcap_handle_);
	if (pcap_fd_ < 0) {
		pcap_close(pcap_handle_);
		pcap_handle_ = nullptr;
		return LogMsg("Cannot get selectable fd");
	}

	// Wrap in Boost.Asio descriptor
	try {
		async_fd_ = std::make_unique<boost::asio::posix::stream_descriptor>(io_context_, pcap_fd_);
	} catch (const std::exception &e) {
		pcap_close(pcap_handle_);
		pcap_handle_ = nullptr;
		return LogMsg(e.what());
	}

	// Apply BPF filter if provided
	if (!filter_expr.empty()) {
		auto ec = SetFilter(filter_expr);
		if (ec) {
			return ec;
		}
	}

	return boost::system::error_code{};
}


void AsyncRawSocket::AsyncReceive(const receive_handler &handler) {
	if (!async_fd_) {
		handler(LogMsg("Socket not initialized"), nullptr, 0);
		return;
	}

	async_fd_->async_wait(boost::asio::posix::descriptor::wait_read, [this, handler](boost::system::error_code ec) {
		if (ec) {
			handler(ec, nullptr, 0);
			return;
		}

		// Process available packets
		struct pcap_pkthdr *header;
		const u_char *packet;
		int processed = 0;

		// Process multiple packets to reduce callback overhead
		while (processed < BATCH_PROCESS_COUNT_) {
			int res = pcap_next_ex(pcap_handle_, &header, &packet);

			if (res == 1) {
				// Got packet - deliver raw data to handler
				handler(boost::system::error_code{}, packet, header->caplen);
				processed++;
			} else if (res == 0) {
				// No more packets available
				break;
			} else if (res == -1) {
				// Error
				handler(LogMsg(pcap_geterr(pcap_handle_)), nullptr, 0);
				return;
			} else {
				// EOF or break - shouldn't happen in live capture
				break;
			}
		}
		// Continue receiving
		AsyncReceive(handler);
	});
}


boost::system::error_code AsyncRawSocket::Send(const uint8_t *data, size_t length) {
	if (!pcap_handle_) {
		return LogMsg("Socket not initialized");
	}

	// Thread-safe sending
	std::lock_guard<std::mutex> lock(send_mutex_);

	int bytes_sent = pcap_inject(pcap_handle_, data, length);
	if (bytes_sent < 0) {
		return LogMsg(pcap_geterr(pcap_handle_));
	}

	if (static_cast<size_t>(bytes_sent) != length) {
		return LogMsg("Incomplete send");
	}

	return boost::system::error_code{};
}


void AsyncRawSocket::AsyncSend(const uint8_t *data, size_t length, const send_handler &handler) {
	// pcap_inject is synchronous, so we post it to io_context
	boost::asio::post(io_context_, [this, data = std::vector<uint8_t>(data, data + length), handler]() {
		const boost::system::error_code ec = Send(data.data(), data.size());
		handler(ec, ec ? 0 : data.size());
	});
}


boost::system::error_code AsyncRawSocket::SetFilter(const std::string &filter_expr) {
	if (!pcap_handle_) {
		return LogMsg("Socket not initialized");
	}

	struct bpf_program fp{};

	// Compile filter with optimization
	if (pcap_compile(pcap_handle_, &fp, filter_expr.c_str(), 1, PCAP_NETMASK_UNKNOWN) < 0) {
		return LogMsg(pcap_geterr(pcap_handle_));
	}

	if (pcap_setfilter(pcap_handle_, &fp) < 0) {
		pcap_freecode(&fp);
		return LogMsg(pcap_geterr(pcap_handle_));
	}

	pcap_freecode(&fp);
	return boost::system::error_code{};
}


AsyncRawSocket::Stats AsyncRawSocket::GetStats() const {
	Stats stats{};
	if (pcap_handle_) {
		struct pcap_stat ps{};
		if (pcap_stats(pcap_handle_, &ps) == 0) {
			stats.received = ps.ps_recv;
			stats.dropped = ps.ps_drop;
			stats.if_dropped = ps.ps_ifdrop;
		}
	}
	return stats;
}


void AsyncRawSocket::Close() {
	if (async_fd_) {
		async_fd_->release();
		async_fd_.reset();
	}
	if (pcap_handle_) {
		pcap_close(pcap_handle_);
		pcap_handle_ = nullptr;
	}
	pcap_fd_ = -1;
}


bool AsyncRawSocket::is_open() const {
	return pcap_handle_ != nullptr;
}


boost::system::error_code AsyncRawSocket::LogMsg(const std::string &msg) {
	if (!msg.empty()) {
		spdlog::critical("[AsyncRawSocket] {}", msg);
	}
	return boost::system::error_code(boost::system::errc::io_error, boost::system::generic_category());
}
