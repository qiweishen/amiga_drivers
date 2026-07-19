#include "core/signal_stop.hpp"

#include <csignal>
#include <unistd.h>

namespace jai {

	namespace {
		StopController *g_controller = nullptr;
		volatile sig_atomic_t g_signal_count = 0;

		void handle_stop_signal(int) {
			if (++g_signal_count >= 2) {
				// Second Ctrl+C / TERM: force quit. The on-disk format is
				// crash-tolerant; inspect_raw.py rebuild-index recovers the tail.
				_exit(130);
			}
			if (g_controller != nullptr) {
				g_controller->request_stop(StopReason::Signal);
			}
		}
	}  // namespace

	const char *stop_reason_name(StopReason reason) {
		switch (reason) {
			case StopReason::None:
				return "none";
			case StopReason::Signal:
				return "signal";
			case StopReason::LimitReached:
				return "limit_reached";
			case StopReason::Error:
				return "error";
			case StopReason::External:
				return "external";
		}
		return "unknown";
	}

	void install_signal_handlers(StopController *controller) {
		g_controller = controller;

		struct sigaction sa{};
		sa.sa_handler = handle_stop_signal;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;  // no SA_RESTART: blocking syscalls should wake up
		sigaction(SIGINT, &sa, nullptr);
		sigaction(SIGTERM, &sa, nullptr);

		struct sigaction ign{};
		ign.sa_handler = SIG_IGN;
		sigemptyset(&ign.sa_mask);
		sigaction(SIGPIPE, &ign, nullptr);
	}

}  // namespace jai
