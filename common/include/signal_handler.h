#pragma once

#include <atomic>
#include <csignal>
#include <initializer_list>


namespace common {
    class SignalHandler {
    public:
        /// Install signal handlers for the given signals (default: SIGINT, SIGTERM).
    /// `terminate_flag` will be set to true when any of these signals is received.
    /// The flag reference must remain valid for the lifetime of the process (or until
    /// another call to Install() replaces it).
    ///
    /// Async-signal-safe: the handler only performs a relaxed atomic store.
        static void Install(std::atomic<bool> &terminate_flag,
                            std::initializer_list<int> signals = {SIGINT, SIGTERM});

        /// Optional: also capture the most recently received signal number.
    /// `signal_received` will be set to the signal number atomically.
        static void Install(std::atomic<bool> &terminate_flag,
                            std::atomic<int> &signal_received,
                            std::initializer_list<int> signals = {
                                SIGINT, SIGTERM, SIGHUP
                            });

    private:
        static std::atomic<bool> *s_terminate;
        static std::atomic<int> *s_signal_received;

        static void Handler(int sig);
    };
} // namespace common

