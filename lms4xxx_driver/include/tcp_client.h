/// @file lms4xxx_tcp_client.h
/// @brief Synchronous interface over nonblocking Boost.Asio I/O and bounded poll.
/// ShutdownReceive() (SHUT_RD) wakes a pending receive during teardown.
/// Threads: Connect/Disconnect/Write on the control thread, Read/ReadSome on the
/// receive thread; concurrent read + write is safe, read + read is not.

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "app_config.h"


namespace lms4xxx {
    // Own std::error_category ("tcp_client")
    enum class TcpError {
        kSuccess = 0,
        kConnectionFailed = 1, ///< TCP connection could not be established
        kConnectionTimeout = 2, ///< Connection attempt timed out
        kConnectionLost = 3, ///< Unexpected disconnection during operation
        kResponseTimeout = 4, ///< Read deadline exceeded
        kNotConnected = 5, ///< Operation requires an active connection
        kAlreadyConnected = 6, ///< Connect called on an active connection
        kReceiveShutdown = 7, ///< EOF after a local ShutdownReceive request
    };

    const std::error_category &TcpErrorCategory() noexcept;

    inline std::error_code make_error_code(TcpError e) noexcept {
        return {static_cast<int>(e), TcpErrorCategory()};
    }


    class TcpClient {
    public:
        struct Options {
            std::string host;
            std::uint16_t port = 0;

            std::size_t recv_buffer_bytes = 0; // 0 = leave the kernel default

            bool tcp_keepalive = false;
            int keepalive_idle_s = 5;
            int keepalive_interval_s = 1;
            int keepalive_count = 3;

            // ReadSome idle wait, not the full Read response deadline.
            // Values <= 0 are clamped to 1 ms; no unbounded data I/O.
            int recv_timeout_ms = 100;

            // Absolute Write deadline; <= 0 is clamped to 1 ms. A timeout
            // invalidates the connection: a partial command cannot be retried.
            int send_timeout_ms = 1000;
        };

        explicit TcpClient(Options options);

        ~TcpClient();

        TcpClient(const TcpClient &) = delete;

        TcpClient &operator=(const TcpClient &) = delete;

        // Connect with a deadline, then apply the socket options
        std::error_code Connect(int timeout_ms);

        // SHUT_RD: unblocks a pending read from another thread; EOF is reported as
        // kReceiveShutdown, not connection loss. The send side stays open.
        void ShutdownReceive();

        // Idempotent
        void Disconnect();

        bool IsConnected() const;

        // Exactly `len` bytes. `timeout_ms` must be > 0; past the deadline the
        // call reports kResponseTimeout and RETURNS THE BYTES ALREADY READ, so
        // the caller can tell "nothing arrived" (0) from "half an answer is on
        // the floor" (> 0) and resynchronise instead of silently reading the
        // tail of this answer as the head of the next one.
        // A local receive shutdown also returns the bytes already read.
        std::size_t Read(std::uint8_t *buf, std::size_t len, std::error_code &ec, int timeout_ms);

        // Whatever is available; 0 without error when the idle wait expires,
        // or kReceiveShutdown when EOF follows a local receive shutdown.
        std::size_t ReadSome(std::uint8_t *buf, std::size_t max_len, std::error_code &ec);

        // Sends all `len` bytes
        std::error_code Write(const std::uint8_t *data, std::size_t len);

        std::error_code Write(const std::vector<std::uint8_t> &data);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };


    inline TcpClient::Options MakeTcpOptions(const DeviceConfig &device,
                                                           const NetworkConfig &network) {
        TcpClient::Options opts;
        opts.host = device.ip;
        opts.port = device.port;
        opts.recv_buffer_bytes = network.recv_buffer_bytes;
        opts.tcp_keepalive = network.tcp_keepalive;
        opts.keepalive_idle_s = network.keepalive_idle_s;
        opts.keepalive_interval_s = network.keepalive_interval_s;
        opts.keepalive_count = network.keepalive_count;
        opts.recv_timeout_ms = std::min(network.response_timeout_ms, 100);
        opts.send_timeout_ms = network.response_timeout_ms;
        return opts;
    }
} // namespace lms4xxx


namespace std {
    template<>
    struct is_error_code_enum<lms4xxx::TcpError> : true_type {
    };
} // namespace std
