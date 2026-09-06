#include "signal_handler.h"


namespace common {
    // Static storage for signal handler state.
    std::atomic<bool> *SignalHandler::s_terminate = nullptr;
    std::atomic<int> *SignalHandler::s_signal_received = nullptr;


    void SignalHandler::Handler(int sig) {
        if (s_terminate) {
            s_terminate->store(true, std::memory_order_release);
        }
        if (s_signal_received) {
            s_signal_received->store(sig, std::memory_order_relaxed);
        }
    }


    void SignalHandler::Install(std::atomic<bool> &terminate_flag,
                                std::initializer_list<int> signals) {
        s_terminate = &terminate_flag;
        s_signal_received = nullptr;
        for (int sig: signals) {
            std::signal(sig, Handler);
        }
    }


    void SignalHandler::Install(std::atomic<bool> &terminate_flag,
                                std::atomic<int> &signal_received,
                                std::initializer_list<int> signals) {
        s_terminate = &terminate_flag;
        s_signal_received = &signal_received;
        for (int sig: signals) {
            std::signal(sig, Handler);
        }
    }
} // namespace common
