#include "lms4xxx_tcp_client.h"

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstring>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "lms4xxx_error.h"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxTCPClient";
}


namespace LMS4xxx {

	namespace asio = boost::asio;
	using tcp = asio::ip::tcp;


	struct TcpClient::Impl {
		DeviceConfig device_config;
		NetworkConfig network_config;

		asio::io_context io_context;
		tcp::socket socket;
		std::atomic<bool> connected{ false };

		explicit Impl(const DeviceConfig &dev_cfg, const NetworkConfig &net_cfg) :
			device_config(dev_cfg), network_config(net_cfg), io_context(), socket(io_context) {}

		// Configure socket options after a successful connect.
		void ConfigureSocket() {
			auto native_fd = socket.native_handle();
			boost::system::error_code ec;

			// Disable Nagle's algorithm for low-latency command sending.
			socket.set_option(tcp::no_delay(true), ec);
			if (ec) {
				Common::Log::log_message(spdlog::level::warn, kModule, "Failed to set TCP_NODELAY", ec.message());
			}

			// Set SO_RCVBUF >= 4 MB for zero-loss at 600 Hz.
			int recv_buf = static_cast<int>(network_config.recv_buffer_bytes);
			if (setsockopt(native_fd, SOL_SOCKET, SO_RCVBUF, &recv_buf, sizeof(recv_buf)) < 0) {
				Common::Log::log_message(spdlog::level::warn, kModule,
										 fmt::format("Failed to set SO_RCVBUF to {} bytes: {}", recv_buf, std::strerror(errno)));
			} else {
				// Read back actual value (kernel may double it).
				int actual = 0;
				socklen_t optlen = sizeof(actual);
				getsockopt(native_fd, SOL_SOCKET, SO_RCVBUF, &actual, &optlen);
				Common::Log::log_message(spdlog::level::trace, kModule,
										 fmt::format("SO_RCVBUF set to {} bytes (requested {})", actual, recv_buf));
			}

			// Enable TCP keepalive.
			if (network_config.tcp_keepalive) {
				int optval = 1;
				if (setsockopt(native_fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
					Common::Log::log_message(spdlog::level::warn, kModule, "Failed to enable SO_KEEPALIVE", std::strerror(errno));
				}

				int idle = network_config.keepalive_idle_s;
				if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0) {
					Common::Log::log_message(spdlog::level::warn, kModule, "Failed to set TCP_KEEPIDLE", std::strerror(errno));
				}

				int interval = network_config.keepalive_interval_s;
				if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0) {
					Common::Log::log_message(spdlog::level::warn, kModule, "Failed to set TCP_KEEPINTVL", std::strerror(errno));
				}

				int count = network_config.keepalive_count;
				if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) {
					Common::Log::log_message(spdlog::level::warn, kModule, "Failed to set TCP_KEEPCNT", std::strerror(errno));
				}

				Common::Log::log_message(
						spdlog::level::trace, kModule,
						fmt::format("TCP keepalive enabled (idle={}s, interval={}s, count={})", idle, interval, count));
			}

			// Set receive timeout so ReadSome() wakes up periodically.
			// This allows the receive thread to check its stop flag on shutdown,
			// preventing indefinite blocking when the device pauses streaming (e.g. NTP sync).
			{
				struct timeval tv{};
				tv.tv_sec = 0;
				tv.tv_usec = 100000;  // 100 ms
				if (setsockopt(native_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
					Common::Log::log_message(spdlog::level::warn, kModule,
											 fmt::format("Failed to set SO_RCVTIMEO: {}", std::strerror(errno)));
				}
			}
		}
	};


	TcpClient::TcpClient(const DeviceConfig &device_config, const NetworkConfig &network_config) :
		impl_(std::make_unique<Impl>(device_config, network_config)) {}


	TcpClient::~TcpClient() {
		if (impl_ && impl_->connected.load(std::memory_order_acquire)) {
			Disconnect();
		}
	}


	TcpClient::TcpClient(TcpClient &&) noexcept = default;
	TcpClient &TcpClient::operator=(TcpClient &&) noexcept = default;


	std::error_code TcpClient::Connect(int timeout_ms) {
		if (impl_->connected.load(std::memory_order_acquire)) {
			return make_error_code(ErrorCode::kAlreadyConnected);
		}

		Common::Log::log_message(
				spdlog::level::trace, kModule,
				fmt::format("Connecting to {}:{} (timeout {}ms)", impl_->device_config.ip, impl_->device_config.port, timeout_ms));

		// Resolve endpoint.
		boost::system::error_code bec;
		auto endpoints =
				tcp::resolver(impl_->io_context).resolve(impl_->device_config.ip, std::to_string(impl_->device_config.port), bec);

		if (bec) {
			Common::Log::log_and_throw(kModule,
									   fmt::format("Failed to resolve {}:{}", impl_->device_config.ip, impl_->device_config.port),
									   bec.message(), false);
			return make_error_code(ErrorCode::kConnectionFailed);
		}

		// Async connect with timeout.
		bool connect_done = false;
		boost::system::error_code connect_ec;

		// Ensure fresh io_context state.
		impl_->io_context.restart();

		impl_->socket.async_connect(endpoints->endpoint(), [&](const boost::system::error_code &ec) {
			connect_ec = ec;
			connect_done = true;
		});

		// Run io_context with a deadline.
		impl_->io_context.run_for(std::chrono::milliseconds(timeout_ms));

		if (!connect_done) {
			// Timeout — cancel pending operation and close socket.
			impl_->socket.close(bec);
			Common::Log::log_and_throw(kModule, fmt::format("Connection timed out after {}ms", timeout_ms), "", false);
			return make_error_code(ErrorCode::kConnectionTimeout);
		}

		if (connect_ec) {
			Common::Log::log_and_throw(kModule, "Connection failed", connect_ec.message(), false);
			return make_error_code(ErrorCode::kConnectionFailed);
		}

		// Configure socket options.
		impl_->ConfigureSocket();

		impl_->connected.store(true, std::memory_order_release);
		Common::Log::log_message(spdlog::level::trace, kModule, fmt::format("Connected to {}", RemoteEndpointStr()));
		return {};
	}


	void TcpClient::Disconnect() {
		if (!impl_ || !impl_->connected.load(std::memory_order_acquire)) {
			return;
		}

		boost::system::error_code ec;
		impl_->socket.shutdown(tcp::socket::shutdown_both, ec);
		impl_->socket.close(ec);
		impl_->connected.store(false, std::memory_order_release);

		Common::Log::log_message(spdlog::level::trace, kModule, "Disconnected");
	}


	bool TcpClient::IsConnected() const {
		return impl_ && impl_->connected.load(std::memory_order_acquire);
	}


	std::size_t TcpClient::Read(std::uint8_t *buf, std::size_t len, std::error_code &ec) {
		if (!impl_->connected.load(std::memory_order_acquire)) {
			ec = make_error_code(ErrorCode::kNotConnected);
			return 0;
		}

		// Manual loop instead of asio::read() to handle SO_RCVTIMEO timeouts.
		// asio::read() treats EAGAIN/would_block as a fatal error and aborts,
		// so we loop with read_some() and retry on timeout.
		std::size_t total = 0;
		while (total < len) {
			boost::system::error_code bec;
			std::size_t n = impl_->socket.read_some(
				asio::buffer(buf + total, len - total), bec);
			total += n;

			if (bec) {
				if (bec == asio::error::try_again ||
					bec == asio::error::would_block) {
					continue;  // SO_RCVTIMEO expired, retry
				}
				if (bec == asio::error::eof || bec == asio::error::connection_reset) {
					impl_->connected.store(false, std::memory_order_release);
					ec = make_error_code(ErrorCode::kConnectionLost);
					Common::Log::log_message(spdlog::level::warn, kModule, "Connection lost during read", bec.message());
				} else {
					ec = make_error_code(ErrorCode::kConnectionFailed);
					Common::Log::log_message(spdlog::level::warn, kModule, "Read error", bec.message());
				}
				return 0;
			}
		}

		ec = {};
		return total;
	}


	std::size_t TcpClient::ReadSome(std::uint8_t *buf, std::size_t max_len, std::error_code &ec) {
		if (!impl_->connected.load(std::memory_order_acquire)) {
			ec = make_error_code(ErrorCode::kNotConnected);
			return 0;
		}

		boost::system::error_code bec;
		std::size_t bytes_read = impl_->socket.read_some(asio::buffer(buf, max_len), bec);

		if (bec) {
			// SO_RCVTIMEO timeout: treat as "no data yet, not an error".
			// The receive loop will re-check its stop flag and retry.
			if (bec == boost::asio::error::try_again ||
				bec == boost::asio::error::would_block) {
				ec = {};
				return 0;
			}
			if (bec == asio::error::eof || bec == asio::error::connection_reset) {
				impl_->connected.store(false, std::memory_order_release);
				ec = make_error_code(ErrorCode::kConnectionLost);
				Common::Log::log_message(spdlog::level::warn, kModule, "Connection lost during read_some", bec.message());
			} else {
				ec = make_error_code(ErrorCode::kConnectionFailed);
				Common::Log::log_message(spdlog::level::warn, kModule, "read_some error", bec.message());
			}
			return 0;
		}

		ec = {};
		return bytes_read;
	}


	std::error_code TcpClient::Write(const std::uint8_t *data, std::size_t len) {
		if (!impl_->connected.load(std::memory_order_acquire)) {
			return make_error_code(ErrorCode::kNotConnected);
		}

		boost::system::error_code bec;
		asio::write(impl_->socket, asio::buffer(data, len), bec);

		if (bec) {
			if (bec == asio::error::eof || bec == asio::error::connection_reset || bec == asio::error::broken_pipe) {
				impl_->connected.store(false, std::memory_order_release);
				Common::Log::log_message(spdlog::level::warn, kModule, "Connection lost during write", bec.message());
				return make_error_code(ErrorCode::kConnectionLost);
			}
			Common::Log::log_message(spdlog::level::warn, kModule, "Write error", bec.message());
			return make_error_code(ErrorCode::kConnectionFailed);
		}

		return {};
	}


	std::error_code TcpClient::Write(const std::vector<std::uint8_t> &data) {
		return Write(data.data(), data.size());
	}


	std::string TcpClient::RemoteEndpointStr() const {
		if (!impl_ || !impl_->connected.load(std::memory_order_acquire)) {
			return "not connected";
		}

		boost::system::error_code ec;
		auto ep = impl_->socket.remote_endpoint(ec);
		if (ec) {
			return "unknown";
		}
		return ep.address().to_string() + ":" + std::to_string(ep.port());
	}

}  // namespace LMS4xxx
