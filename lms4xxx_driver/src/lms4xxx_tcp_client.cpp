#include "lms4xxx_tcp_client.h"

#include <atomic>
#include <boost/asio.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/tcp.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>

#include "logger.h"


namespace {
    constexpr std::string_view kModule = "LMS4xxx";
    Common::DriverLog g_log{std::string(kModule)};


    class TcpErrorCategoryImpl final : public std::error_category {
    public:
        [[nodiscard]] const char *name() const noexcept override { return "tcp_client"; }

        [[nodiscard]] std::string message(int ev) const override {
            switch (static_cast<LMS4xxx::TcpError>(ev)) {
                case LMS4xxx::TcpError::kSuccess:
                    return "success";
                case LMS4xxx::TcpError::kConnectionFailed:
                    return "TCP connection failed";
                case LMS4xxx::TcpError::kConnectionTimeout:
                    return "connection timed out";
                case LMS4xxx::TcpError::kConnectionLost:
                    return "connection lost unexpectedly";
                case LMS4xxx::TcpError::kResponseTimeout:
                    return "response timeout";
                case LMS4xxx::TcpError::kNotConnected:
                    return "not connected";
                case LMS4xxx::TcpError::kAlreadyConnected:
                    return "already connected";
            }
            return "unknown tcp_client error";
        }
    };
} // namespace


namespace LMS4xxx {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;


    const std::error_category &TcpErrorCategory() noexcept {
        static TcpErrorCategoryImpl instance;
        return instance;
    }


    struct TCPClient::Impl {
        Options options;

        asio::io_context io_context;
        tcp::socket socket;
        std::atomic<bool> connected{false};

        explicit Impl(Options opts) : options(std::move(opts)), io_context(), socket(io_context) {
        }

        // Configure socket options after a successful connect.
        void ConfigureSocket() {
            auto native_fd = socket.native_handle();
            boost::system::error_code ec;

            // Disable Nagle's algorithm for low-latency command sending.
            socket.set_option(tcp::no_delay(true), ec);
            if (ec) {
                g_log.warn("[TCP] Failed to set TCP_NODELAY: {}", ec.message());
            }

            // SO_RCVBUF (e.g. >= 4 MB for zero-loss at 600 Hz LiDAR streams).
            if (options.recv_buffer_bytes > 0) {
                int recv_buf = static_cast<int>(options.recv_buffer_bytes);
                if (setsockopt(native_fd, SOL_SOCKET, SO_RCVBUF, &recv_buf, sizeof(recv_buf)) < 0) {
                    g_log.warn("[TCP] Failed to set SO_RCVBUF to {} bytes: {}", recv_buf, std::strerror(errno));
                } else {
                    // Read back actual value (kernel may double it).
                    int actual = 0;
                    socklen_t optlen = sizeof(actual);
                    getsockopt(native_fd, SOL_SOCKET, SO_RCVBUF, &actual, &optlen);
                    g_log.trace("[TCP] SO_RCVBUF set to {} bytes (requested {})", actual, recv_buf);
                }
            }

            // Enable TCP keepalive.
            if (options.tcp_keepalive) {
                int optval = 1;
                if (setsockopt(native_fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
                    g_log.warn("[TCP] Failed to enable SO_KEEPALIVE: {}", std::strerror(errno));
                }

                int idle = options.keepalive_idle_s;
                if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0) {
                    g_log.warn("[TCP] Failed to set TCP_KEEPIDLE: {}", std::strerror(errno));
                }

                int interval = options.keepalive_interval_s;
                if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0) {
                    g_log.warn("[TCP] Failed to set TCP_KEEPINTVL: {}", std::strerror(errno));
                }

                int count = options.keepalive_count;
                if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) {
                    g_log.warn("[TCP] Failed to set TCP_KEEPCNT: {}", std::strerror(errno));
                }

                g_log.trace("[TCP] TCP keepalive enabled (idle={}s, interval={}s, count={})", idle, interval, count);
            }

            // Set receive timeout so ReadSome() wakes up periodically.
            // This allows the receive thread to check its stop flag on shutdown,
            // preventing indefinite blocking when the device pauses streaming (e.g. NTP sync).
            if (options.recv_timeout_ms > 0) {
                struct timeval tv{};
                tv.tv_sec = options.recv_timeout_ms / 1000;
                tv.tv_usec = (options.recv_timeout_ms % 1000) * 1000;
                if (setsockopt(native_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
                    g_log.warn("[TCP] Failed to set SO_RCVTIMEO: {}", std::strerror(errno));
                }
            }

            // Bound blocking sends (a peer that stops reading would otherwise
            // stall Write() for the full TCP retransmission timeout)
            if (options.send_timeout_ms > 0) {
                struct timeval tv{};
                tv.tv_sec = options.send_timeout_ms / 1000;
                tv.tv_usec = (options.send_timeout_ms % 1000) * 1000;
                if (setsockopt(native_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
                    g_log.warn("[TCP] Failed to set SO_SNDTIMEO: {}", std::strerror(errno));
                }
            }
        }
    };


    TCPClient::TCPClient(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {
    }


    TCPClient::~TCPClient() {
        if (impl_ && impl_->connected.load(std::memory_order_acquire)) {
            Disconnect();
        }
    }


    TCPClient::TCPClient(TCPClient &&) noexcept = default;

    TCPClient &TCPClient::operator=(TCPClient &&) noexcept = default;


    std::error_code TCPClient::Connect(int timeout_ms) {
        if (impl_->connected.load(std::memory_order_acquire)) {
            return make_error_code(TcpError::kAlreadyConnected);
        }

        // Resolve endpoint.
        boost::system::error_code bec;
        auto endpoints = tcp::resolver(impl_->io_context).resolve(impl_->options.host,
                                                                  std::to_string(impl_->options.port), bec);

        if (bec) {
            g_log.error("[TCP] Failed to resolve {}:{}: {}", impl_->options.host, impl_->options.port, bec.message());
            return make_error_code(TcpError::kConnectionFailed);
        }

        // Async connect with timeout.
        bool connect_done = false;
        boost::system::error_code connect_ec;

        // Ensure fresh io_context state.
        impl_->io_context.restart();

        // Range overload: tries every resolved endpoint in order (IPv6/IPv4
        // fallback, multiple A records)
        asio::async_connect(impl_->socket, endpoints,
                            [&](const boost::system::error_code &ec, const tcp::endpoint &) {
                                connect_ec = ec;
                                connect_done = true;
                            });

        // Run io_context with a deadline.
        impl_->io_context.run_for(std::chrono::milliseconds(timeout_ms));

        if (!connect_done) {
            // Timeout — cancel pending operation and close socket.
            impl_->socket.close(bec);
            g_log.error("[TCP] Connection timed out after {}ms", timeout_ms);
            return make_error_code(TcpError::kConnectionTimeout);
        }

        if (connect_ec) {
            g_log.error("[TCP] Connection failed: {}", connect_ec.message());
            return make_error_code(TcpError::kConnectionFailed);
        }

        // Configure socket options.
        impl_->ConfigureSocket();

        impl_->connected.store(true, std::memory_order_release);
        return {};
    }


    void TCPClient::ShutdownReceive() {
        if (!impl_ || !impl_->connected.load(std::memory_order_acquire)) {
            return;
        }
        boost::system::error_code ec;
        impl_->socket.shutdown(tcp::socket::shutdown_receive, ec);
        // Best-effort: ignore error (socket may already be closing)
    }


    void TCPClient::Disconnect() {
        if (!impl_ || !impl_->connected.load(std::memory_order_acquire)) {
            return;
        }

        boost::system::error_code ec;
        impl_->socket.shutdown(tcp::socket::shutdown_both, ec);
        impl_->socket.close(ec);
        impl_->connected.store(false, std::memory_order_release);

        g_log.trace("[TCP] Disconnected");
    }


    bool TCPClient::IsConnected() const {
        return impl_ && impl_->connected.load(std::memory_order_acquire);
    }


    std::size_t TCPClient::Read(std::uint8_t *buf, std::size_t len, std::error_code &ec, int timeout_ms) {
        if (!impl_->connected.load(std::memory_order_acquire)) {
            ec = make_error_code(TcpError::kNotConnected);
            return 0;
        }

        // Manual loop instead of asio::read() to handle SO_RCVTIMEO timeouts.
        // asio::read() treats EAGAIN/would_block as a fatal error and aborts,
        // so we loop with read_some() and retry on timeout.
        const bool has_deadline = timeout_ms > 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        std::size_t total = 0;
        while (total < len) {
            boost::system::error_code bec;
            std::size_t n = impl_->socket.read_some(asio::buffer(buf + total, len - total), bec);
            total += n;

            if (bec) {
                if (bec == asio::error::try_again || bec == asio::error::would_block) {
                    // SO_RCVTIMEO expired — check deadline before retrying.
                    if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
                        ec = make_error_code(TcpError::kResponseTimeout);
                        g_log.warn("[TCP] Read deadline exceeded ({}ms, got {}/{} bytes)", timeout_ms, total, len);
                        return 0;
                    }
                    continue;
                }
                if (bec == asio::error::eof || bec == asio::error::connection_reset) {
                    impl_->connected.store(false, std::memory_order_release);
                    ec = make_error_code(TcpError::kConnectionLost);
                    g_log.warn("[TCP] Connection lost during read: {}", bec.message());
                } else {
                    ec = make_error_code(TcpError::kConnectionFailed);
                    g_log.warn("[TCP] Read error: {}", bec.message());
                }
                return 0;
            }
        }

        ec = {};
        return total;
    }


    std::size_t TCPClient::ReadSome(std::uint8_t *buf, std::size_t max_len, std::error_code &ec) {
        if (!impl_->connected.load(std::memory_order_acquire)) {
            ec = make_error_code(TcpError::kNotConnected);
            return 0;
        }

        boost::system::error_code bec;
        std::size_t bytes_read = impl_->socket.read_some(asio::buffer(buf, max_len), bec);

        if (bec) {
            // SO_RCVTIMEO timeout: treat as "no data yet, not an error".
            // The receive loop will re-check its stop flag and retry.
            if (bec == boost::asio::error::try_again || bec == boost::asio::error::would_block) {
                ec = {};
                return 0;
            }
            if (bec == asio::error::eof || bec == asio::error::connection_reset) {
                impl_->connected.store(false, std::memory_order_release);
                ec = make_error_code(TcpError::kConnectionLost);
                g_log.warn("[TCP] Connection lost during read_some: {}", bec.message());
            } else {
                ec = make_error_code(TcpError::kConnectionFailed);
                g_log.warn("[TCP] read_some error: {}", bec.message());
            }
            return 0;
        }

        ec = {};
        return bytes_read;
    }


    std::error_code TCPClient::Write(const std::uint8_t *data, std::size_t len) {
        if (!impl_->connected.load(std::memory_order_acquire)) {
            return make_error_code(TcpError::kNotConnected);
        }

        boost::system::error_code bec;
        asio::write(impl_->socket, asio::buffer(data, len), bec);

        if (bec) {
            if (bec == asio::error::eof || bec == asio::error::connection_reset || bec == asio::error::broken_pipe) {
                impl_->connected.store(false, std::memory_order_release);
                g_log.warn("[TCP] Connection lost during write: {}", bec.message());
                return make_error_code(TcpError::kConnectionLost);
            }
            g_log.warn("[TCP] Write error: {}", bec.message());
            return make_error_code(TcpError::kConnectionFailed);
        }

        return {};
    }


    std::error_code TCPClient::Write(const std::vector<std::uint8_t> &data) {
        return Write(data.data(), data.size());
    }


    std::string TCPClient::RemoteEndpointStr() const {
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
} // namespace LMS4xxx
