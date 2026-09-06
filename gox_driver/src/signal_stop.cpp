#include "signal_stop.h"

#include <csignal>
#include <unistd.h>

namespace gox {
    namespace {
        StopController *g_controller = nullptr;
        volatile sig_atomic_t g_signal_count = 0;

        void HandleStopSignal(int) {
            // No compound assignment on volatile (deprecated since C++20); a
            // plain read-modify-write is fine for sig_atomic_t in a handler
            g_signal_count = g_signal_count + 1;
            if (g_signal_count >= 2) {
                // Second Ctrl+C / TERM: force quit. The on-disk format is
                // crash-tolerant; inspect_raw.py rebuild-index recovers the tail.
                _exit(130);
            }
            if (g_controller != nullptr) {
                g_controller->RequestStop(StopReason::kSignal);
            }
        }
    } // namespace

    const char *StopReasonName(StopReason reason) {
        switch (reason) {
            case StopReason::kNone:
                return "none";
            case StopReason::kSignal:
                return "signal";
            case StopReason::kLimitReached:
                return "limit_reached";
            case StopReason::kError:
                return "error";
            case StopReason::kExternal:
                return "external";
        }
        return "unknown";
    }

    void InstallSignalHandlers(StopController *controller) {
        g_controller = controller;

        struct sigaction sa{};
        sa.sa_handler = HandleStopSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; // no SA_RESTART: blocking syscalls should wake up
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        struct sigaction ign{};
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        sigaction(SIGPIPE, &ign, nullptr);
    }
} // namespace gox
