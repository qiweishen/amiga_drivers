/// @file tcp_client.h
/// @brief Shared synchronous TCP client (Boost.Asio behind a Pimpl).
///
/// Originated as the LMS4xxx client; the socket tuning and the shutdown
/// sequencing are field-tested — keep the semantics:
///  - SO_RCVTIMEO (default 100 ms) makes ReadSome() wake up periodically so a
///    receive loop can re-check its stop flag.
///  - ShutdownReceive() uses SHUT_RD to wake a ReadSome() blocked inside
///    epoll (SO_RCVTIMEO does not apply there) from another thread.
///
/// Thread safety:
///   - Connect()/Disconnect() must be called from the control thread.
///   - Read()/ReadSome() are called from the receive thread.
///   - Write() may be called from the control thread (command sending).
///   - Concurrent read + write is safe (Boost.Asio full-duplex).
///   - Concurrent read + read or write + write is NOT safe.

#ifndef COMMON_TCP_CLIENT_H
#define COMMON_TCP_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>


namespace Common {

	// TcpClient error codes with their own std::error_category ("tcp_client")
	enum class TcpError {
		kSuccess = 0,
		kConnectionFailed = 1,	 ///< TCP connection could not be established
		kConnectionTimeout = 2,	 ///< Connection attempt timed out
		kConnectionLost = 3,	 ///< Unexpected disconnection during operation
		kResponseTimeout = 4,	 ///< Read deadline exceeded
		kNotConnected = 5,		 ///< Operation requires an active connection
		kAlreadyConnected = 6,	 ///< Connect called on an active connection
	};

	[[nodiscard]] const std::error_category &TcpErrorCategory() noexcept;

	[[nodiscard]] inline std::error_code make_error_code(TcpError e) noexcept {
		return { static_cast<int>(e), TcpErrorCategory() };
	}


	class TcpClient {
	public:
		struct Options {
			std::string host;
			std::uint16_t port = 0;

			std::size_t recv_buffer_bytes = 0;	// 0 = leave the kernel default

			bool tcp_keepalive = false;
			int keepalive_idle_s = 5;
			int keepalive_interval_s = 1;
			int keepalive_count = 3;

			// SO_RCVTIMEO: lets ReadSome() wake up periodically so the receive
			// thread can check its stop flag (0 = blocking reads)
			int recv_timeout_ms = 100;

			// SO_SNDTIMEO: bounds Write() when the peer stops reading and the send
			// buffer fills (0 = blocking sends). A timed-out Write reports
			// TcpError::kConnectionFailed
			int send_timeout_ms = 0;
		};

		explicit TcpClient(Options options);
		~TcpClient();

		// Non-copyable, movable.
		TcpClient(const TcpClient &) = delete;
		TcpClient &operator=(const TcpClient &) = delete;
		TcpClient(TcpClient &&) noexcept;
		TcpClient &operator=(TcpClient &&) noexcept;

		// Connect with a deadline; configures the socket options afterwards
		[[nodiscard]] std::error_code Connect(int timeout_ms);

		// Shut down the receive side of the socket to unblock any pending ReadSome()
		// call in the receive thread. The send side remains open.
		// Safe to call from a different thread than the one calling ReadSome().
		void ShutdownReceive();

		// Close the connection. Safe to call multiple times.
		void Disconnect();

		[[nodiscard]] bool IsConnected() const;

		// Synchronous blocking read of exactly `len` bytes. If `timeout_ms` > 0,
		// returns TcpError::kResponseTimeout past the deadline; 0 = no deadline
		[[nodiscard]] std::size_t Read(std::uint8_t *buf, std::size_t len, std::error_code &ec, int timeout_ms = 0);

		// Synchronous non-blocking read. Returns whatever data is available
		// (0 with no error when SO_RCVTIMEO expired without data)
		[[nodiscard]] std::size_t ReadSome(std::uint8_t *buf, std::size_t max_len, std::error_code &ec);

		// Synchronous write. Sends all `len` bytes
		[[nodiscard]] std::error_code Write(const std::uint8_t *data, std::size_t len);

		[[nodiscard]] std::error_code Write(const std::vector<std::uint8_t> &data);

		// Get the remote endpoint string (for logging)
		[[nodiscard]] std::string RemoteEndpointStr() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

}  // namespace Common


namespace std {
	template<>
	struct is_error_code_enum<Common::TcpError> : true_type {};
}  // namespace std

#endif	// COMMON_TCP_CLIENT_H
