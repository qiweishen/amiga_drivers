#pragma once

#include <atomic>

namespace gox {
    enum class StopReason : int {
        kNone = 0,
        kSignal = 1, // SIGINT/SIGTERM
        kLimitReached = 2, // max_frames / max_duration_s
        kError = 3, // link lost, I/O failure, ...
        kExternal = 4, // host application requested stop (unified mode)
    };

    // Process-wide stop flag. Signal handlers only ever call RequestStop(),
    // which is async-signal-safe (single atomic store). A second SIGINT/SIGTERM
    // while stopping calls _exit(130): segment files stay scannable by design.
    class StopController {
    public:
        void RequestStop(StopReason reason) {
            if (reason == StopReason::kError) {
                reason_.store(static_cast<int>(reason));
                return;
            }
            int expected = 0;
            reason_.compare_exchange_strong(expected, static_cast<int>(reason));
        }

        bool StopRequested() const { return reason_.load(std::memory_order_relaxed) != 0; }
        StopReason Reason() const { return static_cast<StopReason>(reason_.load(std::memory_order_relaxed)); }

    private:
        std::atomic<int> reason_{0};
    };

    const char *StopReasonName(StopReason reason);

    // Installs SIGINT/SIGTERM handlers targeting the given controller (must
    // outlive the handlers, i.e. effectively the whole process). SIGPIPE is set
    // to ignore. Only call once, from the main thread, before spawning threads.
    void InstallSignalHandlers(StopController *controller);
} // namespace gox
