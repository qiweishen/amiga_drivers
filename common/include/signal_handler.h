/// @file common/signal_handler.h
/// @brief Shared POSIX signal handling utility for all Amiga drivers.
///
/// Both the INS401 and LMS41xxx drivers implement nearly identical signal handling:
/// SIGINT, SIGTERM, etc. → store to an atomic flag.
///
/// This header provides a single shared implementation using a function-local
/// static to hold the terminate flag, installed once via install().
///
/// Usage:
///   // In main():
///   std::atomic<bool> terminate{false};
///   Common::SignalHandler::install(terminate);
///   // terminate.load() becomes true when a signal arrives.

#ifndef COMMON_SIGNAL_HANDLER_H
#define COMMON_SIGNAL_HANDLER_H

#include <atomic>
#include <csignal>
#include <initializer_list>


namespace Common {
    class SignalHandler {
    public:
        /// Install signal handlers for the given signals (default: SIGINT, SIGTERM).
    /// `terminate_flag` will be set to true when any of these signals is received.
    /// The flag reference must remain valid for the lifetime of the process (or until
    /// another call to install() replaces it).
    ///
    /// Async-signal-safe: the handler only performs a relaxed atomic store.
        static void install(std::atomic<bool> &terminate_flag,
                            std::initializer_list<int> signals = {SIGINT, SIGTERM});

        /// Optional: also capture the most recently received signal number.
    /// `signal_received` will be set to the signal number atomically.
        static void install(std::atomic<bool> &terminate_flag,
                            std::atomic<int> &signal_received,
                            std::initializer_list<int> signals = {
                                SIGINT, SIGTERM, SIGABRT,
                                SIGTSTP, SIGHUP
                            });

    private:
        static std::atomic<bool> *s_terminate;
        static std::atomic<int> *s_signal_received;

        static void handler(int sig);
    };
} // namespace Common


#endif // COMMON_SIGNAL_HANDLER_H
