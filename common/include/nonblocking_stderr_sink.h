#pragma once

// Console sink that never blocks the logging thread.
//
// stderr of the AmigaDrivers process is a pipe held by the web GUI (or by
// `docker exec`). If that reader stalls, a blocking write would stall every
// driver thread that logs — the acquisition threads included — and the queues
// behind them would overflow. On a non-TTY stderr the descriptor is switched to
// O_NONBLOCK; a message that does not fit into the pipe is dropped and counted.
// The file sink keeps the complete log (the GUI tails that file once a session
// runs, app/services/process.py), so console lines are a convenience only.

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <mutex>

#include <spdlog/sinks/base_sink.h>


namespace common {
    class NonBlockingStderrSink final : public spdlog::sinks::base_sink<std::mutex> {
    public:
        NonBlockingStderrSink() {
            const int flags = ::fcntl(STDERR_FILENO, F_GETFL);
            if (flags >= 0 && (flags & O_NONBLOCK) == 0) {
                ::fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK);
            }
        }

        // Messages dropped (in whole or in part) because the reader did not keep up
        static std::uint64_t DroppedCount() { return dropped_.load(std::memory_order_relaxed); }

    protected:
        void sink_it_(const spdlog::details::log_msg &msg) override {
            spdlog::memory_buf_t formatted;
            base_sink<std::mutex>::formatter_->format(msg, formatted);
            const char *p = formatted.data();
            std::size_t remaining = formatted.size();
            while (remaining > 0) {
                const ssize_t n = ::write(STDERR_FILENO, p, remaining);
                if (n > 0) {
                    p += n;
                    remaining -= static_cast<std::size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                // EAGAIN: the pipe is full (reader stalled); EPIPE/other: no reader.
                // Either way the rest of this message is dropped, never waited for.
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        void flush_() override {
        }

    private:
        inline static std::atomic<std::uint64_t> dropped_{0};
    };
} // namespace common
