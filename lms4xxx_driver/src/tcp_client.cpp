#include "tcp_client.h"

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <boost/asio.hpp>
#include <cerrno>
#include <chrono>
#include <memory>
#include <poll.h>
#include <cstring>
#include <netinet/tcp.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>

#include "logger.h"


namespace {
    constexpr std::string_view kModule = "LMS4xxx";
    common::DriverLog g_log{std::string(kModule)};


    class TcpErrorCategoryImpl final : public std::error_category {
    public:
        [[nodiscard]] const char *name() const noexcept override { return "tcp_client"; }

        [[nodiscard]] std::string message(int ev) const override {
            switch (static_cast<lms4xxx::TcpError>(ev)) {
                case lms4xxx::TcpError::kSuccess:
                    return "success";
                case lms4xxx::TcpError::kConnectionFailed:
                    return "TCP connection failed";
                case lms4xxx::TcpError::kConnectionTimeout:
                    return "connection timed out";
                case lms4xxx::TcpError::kConnectionLost:
                    return "connection lost unexpectedly";
                case lms4xxx::TcpError::kResponseTimeout:
                    return "response timeout";
                case lms4xxx::TcpError::kNotConnected:
                    return "not connected";
                case lms4xxx::TcpError::kAlreadyConnected:
                    return "already connected";
            }
            return "unknown tcp_client error";
        }
    };
} // namespace


namespace lms4xxx {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;


    const std::error_category &TcpErrorCategory() noexcept {
        static TcpErrorCategoryImpl instance;
        return instance;
    }


    struct TcpClient::Impl {
        Options options;

        asio::io_context io_context;
        tcp::socket socket;
        std::atomic<bool> connected{false};

        explicit Impl(Options opts) : options(std::move(opts)), io_context(), socket(io_context) {
        }

        // All data I/O is nonblocking. poll's deadline is a host operational
        // timeout only; it is never a sensor timestamp or a synchronization source.
        std::error_code Wait(short events, std::chrono::steady_clock::time_point deadline) {
            while (connected.load(std::memory_order_acquire)) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    return make_error_code(TcpError::kResponseTimeout);
                }
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                pollfd fd{socket.native_handle(), events, 0};
                const int rc = ::poll(&fd, 1, static_cast<int>(std::min<std::int64_t>(left + 1, 50)));
                if (rc > 0) {
                    if (fd.revents & POLLNVAL) {
                        return make_error_code(TcpError::kConnectionLost);
                    }
                    return {}; // recv/send reports EOF, reset or a pending socket error
                }
                if (rc < 0 && errno != EINTR) {
                    return make_error_code(TcpError::kConnectionFailed);
                }
            }
            return make_error_code(TcpError::kNotConnected);
        }

        void ConfigureSocket() {
            auto native_fd = socket.native_handle();
            boost::system::error_code ec;

            // No Nagle: low-latency commands
            socket.set_option(tcp::no_delay(true), ec);
            if (ec) {
                g_log.Warn("[TCP] Failed to set TCP_NODELAY: {}", ec.message());
            }

            if (options.recv_buffer_bytes > 0) {
                int recv_buf = static_cast<int>(options.recv_buffer_bytes);
                if (setsockopt(native_fd, SOL_SOCKET, SO_RCVBUF, &recv_buf, sizeof(recv_buf)) < 0) {
                    g_log.Warn("[TCP] Failed to set SO_RCVBUF to {} bytes: {}", recv_buf, std::strerror(errno));
                } else {
                    // The kernel may double it
                    int actual = 0;
                    socklen_t optlen = sizeof(actual);
                    getsockopt(native_fd, SOL_SOCKET, SO_RCVBUF, &actual, &optlen);
                    g_log.Trace("[TCP] SO_RCVBUF set to {} bytes (requested {})", actual, recv_buf);
                }
            }

            if (options.tcp_keepalive) {
                int optval = 1;
                if (setsockopt(native_fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
                    g_log.Warn("[TCP] Failed to enable SO_KEEPALIVE: {}", std::strerror(errno));
                }

                int idle = options.keepalive_idle_s;
                if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0) {
                    g_log.Warn("[TCP] Failed to set TCP_KEEPIDLE: {}", std::strerror(errno));
                }

                int interval = options.keepalive_interval_s;
                if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0) {
                    g_log.Warn("[TCP] Failed to set TCP_KEEPINTVL: {}", std::strerror(errno));
                }

                int count = options.keepalive_count;
                if (setsockopt(native_fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) {
                    g_log.Warn("[TCP] Failed to set TCP_KEEPCNT: {}", std::strerror(errno));
                }

                g_log.Trace("[TCP] TCP keepalive enabled (idle={}s, interval={}s, count={})", idle, interval, count);
            }

            // Defensive kernel settings only. Application deadlines are enforced
            // by nonblocking I/O + Wait(), not these socket options.
            if (options.recv_timeout_ms > 0) {
                struct timeval tv{};
                tv.tv_sec = options.recv_timeout_ms / 1000;
                tv.tv_usec = (options.recv_timeout_ms % 1000) * 1000;
                if (setsockopt(native_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
                    g_log.Warn("[TCP] Failed to set SO_RCVTIMEO: {}", std::strerror(errno));
                }
            }

            // Bound Write() against a peer that stops reading
            if (options.send_timeout_ms > 0) {
                struct timeval tv{};
                tv.tv_sec = options.send_timeout_ms / 1000;
                tv.tv_usec = (options.send_timeout_ms % 1000) * 1000;
                if (setsockopt(native_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
                    g_log.Warn("[TCP] Failed to set SO_SNDTIMEO: {}", std::strerror(errno));
                }
            }
        }
    };


    TcpClient::TcpClient(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {
    }


    TcpClient::~TcpClient() {
        if (impl_) {
            Disconnect();
        }
    }


    std::error_code TcpClient::Connect(int timeout_ms) {
        if (impl_->connected.load(std::memory_order_acquire)) {
            return make_error_code(TcpError::kAlreadyConnected);
        }
        Disconnect(); // also closes a socket left open by a failed previous operation

        boost::system::error_code bec;
        const auto address = asio::ip::make_address(impl_->options.host, bec);

        if (bec) {
            g_log.Error("[TCP] Invalid numeric device address: {}", bec.message());
            return make_error_code(TcpError::kConnectionFailed);
        }

        // Shared with the handler: on a timeout it still fires (operation_aborted) after this frame is gone
        struct ConnectState {
            bool done = false;
            boost::system::error_code ec;
        };
        auto state = std::make_shared<ConnectState>();

        impl_->io_context.restart(); // run_for() leaves the context stopped
        impl_->socket.async_connect(tcp::endpoint(address, impl_->options.port),
                            [state](const boost::system::error_code &ec) {
                                state->ec = ec;
                                state->done = true;
                            });
        impl_->io_context.run_for(std::chrono::milliseconds(timeout_ms));

        if (!state->done) {
            impl_->socket.close(bec);
            impl_->io_context.restart();
            impl_->io_context.poll(); // drain the aborted handler now, not inside the next Connect()
            g_log.Error("[TCP] Connection timed out after {}ms", timeout_ms);
            return make_error_code(TcpError::kConnectionTimeout);
        }
        if (state->ec) {
            g_log.Error("[TCP] Connection failed: {}", state->ec.message());
            return make_error_code(TcpError::kConnectionFailed);
        }

        impl_->ConfigureSocket();

        impl_->socket.non_blocking(true, bec);
        if (bec) {
            impl_->socket.close(bec);
            return make_error_code(TcpError::kConnectionFailed);
        }

        impl_->connected.store(true, std::memory_order_release);
        return {};
    }


    void TcpClient::ShutdownReceive() {
        if (!impl_ || !impl_->connected.load(std::memory_order_acquire)) {
            return;
        }
        boost::system::error_code ec;
        impl_->socket.shutdown(tcp::socket::shutdown_receive, ec);
        // Best-effort: the socket may already be closing
    }


    void TcpClient::Disconnect() {
        if (!impl_ || !impl_->socket.is_open()) {
            return;
        }

        boost::system::error_code ec;
        impl_->socket.shutdown(tcp::socket::shutdown_both, ec);
        impl_->socket.close(ec);
        impl_->connected.store(false, std::memory_order_release);

        g_log.Trace("[TCP] Disconnected");
    }


    bool TcpClient::IsConnected() const {
        return impl_ && impl_->connected.load(std::memory_order_acquire);
    }


    std::size_t TcpClient::Read(std::uint8_t *buf, std::size_t len, std::error_code &ec, int timeout_ms) {
        if (!impl_->connected.load(std::memory_order_acquire)) {
            ec = make_error_code(TcpError::kNotConnected);
            return 0;
        }

        // A deadline is mandatory: without one the try_again branch below has no
        // exit and this becomes a 10 Hz busy-wait on the control thread that
        // nothing can interrupt.
        if (timeout_ms <= 0) {
            ec = make_error_code(TcpError::kResponseTimeout);
            g_log.Error("[TCP] Read called without a deadline (timeout_ms={})", timeout_ms);
            return 0;
        }

        // One absolute deadline for the entire read, including partial progress.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        // `total` is returned on every exit path: bytes taken off the wire are gone, and the
        // caller needs the count to realign the stream
        std::size_t total = 0;
        while (total < len) {
            ec = impl_->Wait(POLLIN, deadline);
            if (ec) {
                return total;
            }
            boost::system::error_code bec;
            std::size_t n = impl_->socket.read_some(asio::buffer(buf + total, len - total), bec);
            total += n;

            if (bec) {
                if (bec == asio::error::try_again || bec == asio::error::would_block) {
                    // Readiness may change between poll and a nonblocking read.
                    if (std::chrono::steady_clock::now() >= deadline) {
                        ec = make_error_code(TcpError::kResponseTimeout);
                        g_log.Warn("[TCP] Read deadline exceeded ({}ms, got {}/{} bytes)", timeout_ms, total, len);
                        return total;
                    }
                    continue;
                }
                if (bec == asio::error::eof || bec == asio::error::connection_reset) {
                    impl_->connected.store(false, std::memory_order_release);
                    ec = make_error_code(TcpError::kConnectionLost);
                    g_log.Warn("[TCP] Connection lost during read: {}", bec.message());
                } else {
                    ec = make_error_code(TcpError::kConnectionFailed);
                    g_log.Warn("[TCP] Read error: {}", bec.message());
                }
                return total;
            }
        }

        ec = {};
        return total;
    }


    std::size_t TcpClient::ReadSome(std::uint8_t *buf, std::size_t max_len, std::error_code &ec) {
        if (!impl_->connected.load(std::memory_order_acquire)) {
            ec = make_error_code(TcpError::kNotConnected);
            return 0;
        }

        boost::system::error_code bec;
        ec = impl_->Wait(POLLIN, std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(std::max(1, impl_->options.recv_timeout_ms)));
        if (ec == make_error_code(TcpError::kResponseTimeout)) {
            ec = {};
            return 0;
        }
        if (ec) {
            return 0;
        }
        std::size_t bytes_read = impl_->socket.read_some(asio::buffer(buf, max_len), bec);

        if (bec) {
            // No bytes remain available after the readiness notification.
            if (bec == boost::asio::error::try_again || bec == boost::asio::error::would_block) {
                ec = {};
                return 0;
            }
            if (bec == asio::error::eof || bec == asio::error::connection_reset) {
                impl_->connected.store(false, std::memory_order_release);
                ec = make_error_code(TcpError::kConnectionLost);
                g_log.Warn("[TCP] Connection lost during read_some: {}", bec.message());
            } else {
                ec = make_error_code(TcpError::kConnectionFailed);
                g_log.Warn("[TCP] read_some error: {}", bec.message());
            }
            return 0;
        }

        ec = {};
        return bytes_read;
    }


    std::error_code TcpClient::Write(const std::uint8_t *data, std::size_t len) {
        if (!impl_->connected.load(std::memory_order_acquire)) {
            return make_error_code(TcpError::kNotConnected);
        }

        boost::system::error_code bec;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(std::max(1, impl_->options.send_timeout_ms));
        std::size_t total = 0;
        while (total < len) {
            if (const auto ec = impl_->Wait(POLLOUT, deadline)) {
                // A partial command cannot be retried on the same stream.
                impl_->connected.store(false, std::memory_order_release);
                return ec;
            }
            total += impl_->socket.write_some(asio::buffer(data + total, len - total), bec);
            if (bec == asio::error::try_again || bec == asio::error::would_block) {
                bec.clear();
                continue;
            }
            if (bec) {
                break;
            }
        }

        if (bec) {
            impl_->connected.store(false, std::memory_order_release);
            if (bec == asio::error::eof || bec == asio::error::connection_reset || bec == asio::error::broken_pipe) {
                impl_->connected.store(false, std::memory_order_release);
                g_log.Warn("[TCP] Connection lost during write: {}", bec.message());
                return make_error_code(TcpError::kConnectionLost);
            }
            g_log.Warn("[TCP] Write error: {}", bec.message());
            return make_error_code(TcpError::kConnectionFailed);
        }

        return {};
    }


    std::error_code TcpClient::Write(const std::vector<std::uint8_t> &data) {
        return Write(data.data(), data.size());
    }


} // namespace lms4xxx
