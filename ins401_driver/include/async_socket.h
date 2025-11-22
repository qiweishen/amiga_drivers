#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>



class AsyncPacketSocket : public std::enable_shared_from_this<AsyncPacketSocket> {
public:
	using ReceiveHandler = std::function<void(const uint8_t *, size_t, const boost::system::error_code &)>;
	using SendHandler = std::function<void(const boost::system::error_code &, size_t)>;

	struct Config {
		std::string interface_name;
		size_t block_size = 1 << 19;  // 512KB per block
		size_t frame_size = 2048;	  // 2KB per frame
		uint32_t block_nr = 64;		  // Number of blocks
		uint32_t retire_blk_tov = 1;  // 1ms timeout
		int fanout_group = -1;		  // Fanout group ID (-1 to disable)
		bool promiscuous = false;	  // Promiscuous mode
		int protocol = ETH_P_ALL;	  // Protocol filter
	};

	AsyncPacketSocket(boost::asio::io_context &io_context, Config config) :
		io_context_(io_context), socket_descriptor_(io_context), config_(std::move(config)) {}
	~AsyncPacketSocket() { Close(); }

	bool Open();
	void Close() noexcept;

	void AsyncReceive(ReceiveHandler &&handler);
	void AsyncSend(const uint8_t *data, size_t length, SendHandler &&handler);

	bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

	// Get statistics
	uint64_t GetPacketsReceived() const noexcept { return packets_received_.load(); }
	uint64_t GetPacketsSent() const noexcept { return packets_sent_.load(); }
	uint64_t GetPacketsDropped() const noexcept { return packets_dropped_.load(); }

	// Get kernel statistics
	tpacket_stats_v3 GetKernelStats() const noexcept;


private:
	struct RingBuffer {
		uint8_t *map = nullptr;
		size_t map_size = 0;
		tpacket_req3 req{};
		std::vector<iovec> rd;
		unsigned int current_block = 0;

		void reset() noexcept {
			if (map) {
				::munmap(map, map_size);
				map = nullptr;
			}
			map_size = 0;
			rd.clear();
			current_block = 0;
			req = tpacket_req3{};
		}
	};

	boost::asio::io_context &io_context_;
	boost::asio::posix::stream_descriptor socket_descriptor_;
	Config config_;
	int raw_fd_{ -1 };

	// RX ring buffer
	RingBuffer rx_ring_;
	mutable std::mutex rx_mutex_;

	// TX ring buffer
	RingBuffer tx_ring_;
	mutable std::mutex tx_mutex_;

	// Statistics
	std::atomic<uint64_t> packets_received_{ 0 };
	std::atomic<uint64_t> packets_sent_{ 0 };
	std::atomic<uint64_t> packets_dropped_{ 0 };

	// Async operation flags
	std::atomic<bool> running_{ false };

	bool SetupRXRing();

	bool SetupTXRing();

	bool BindInterface();

	void SetupFanout();

	void SetPromiscuousMode();

	void ProcessRXRing(const ReceiveHandler &handler);

	void SendTXRing(const uint8_t *data, size_t length, const SendHandler &handler);

	void LogMsg(std::string_view msg, spdlog::level::level_enum log_level) const;
};


class SocketGuard {
	int &fd_;
	bool released_ = false;

public:
	explicit SocketGuard(int &fd) : fd_(fd) {}
	~SocketGuard() {
		if (!released_ && fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}
	void release() { released_ = true; }
};
