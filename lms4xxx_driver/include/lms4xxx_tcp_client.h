#ifndef LMS4XXX_TCP_CLIENT_H
#define LMS4XXX_TCP_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "lms4xxx_config.h"


namespace LMS4xxx {

	// TCP client wrapping Boost.Asio for sensor communication.
	//
	// Thread safety:
	//   - Connect()/Disconnect() must be called from the control thread.
	//   - Read()/ReadSome() are called from the receive thread.
	//   - Write() may be called from the control thread (command sending).
	//   - Concurrent read + write is safe (Boost.Asio full-duplex).
	//   - Concurrent read + read or write + write is NOT safe.
	class TcpClient {
	public:
		explicit TcpClient(const DeviceConfig &device_config, const NetworkConfig &network_config);
		~TcpClient();

		// Non-copyable, movable.
		TcpClient(const TcpClient &) = delete;
		TcpClient &operator=(const TcpClient &) = delete;
		TcpClient(TcpClient &&) noexcept;
		TcpClient &operator=(TcpClient &&) noexcept;

		// Connect to the sensor. Configures SO_RCVBUF and TCP keepalive.
		[[nodiscard]] std::error_code Connect(int timeout_ms);

		// Shut down the receive side of the socket to unblock any pending ReadSome()
		// call in the receive thread. The send side remains open.
		// Safe to call from a different thread than the one calling ReadSome().
		void ShutdownReceive();

		// Close the connection. Safe to call multiple times.
		void Disconnect();

		// Check if the socket is connected.
		[[nodiscard]] bool IsConnected() const;

		// Synchronous blocking read. Reads exactly `len` bytes into `buf`.
		[[nodiscard]] std::size_t Read(std::uint8_t *buf, std::size_t len, std::error_code &ec);

		// Synchronous non-blocking read. Returns whatever data is available.
		[[nodiscard]] std::size_t ReadSome(std::uint8_t *buf, std::size_t max_len, std::error_code &ec);

		// Synchronous write. Sends all `len` bytes.
		[[nodiscard]] std::error_code Write(const std::uint8_t *data, std::size_t len);

		// Convenience: write a vector.
		[[nodiscard]] std::error_code Write(const std::vector<std::uint8_t> &data);

		// Get the remote endpoint string (for logging).
		[[nodiscard]] std::string RemoteEndpointStr() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

}  // namespace LMS4xxx

#endif	// LMS4XXX_TCP_CLIENT_H
