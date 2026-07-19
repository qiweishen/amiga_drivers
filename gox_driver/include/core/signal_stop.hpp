#pragma once

#include <atomic>

namespace jai {

	enum class StopReason : int {
		None = 0,
		Signal = 1,		   // SIGINT/SIGTERM
		LimitReached = 2,  // max_frames / max_duration_s
		Error = 3,		   // link lost, I/O failure, ...
		External = 4,	   // host application requested stop (unified mode)
	};

	// Process-wide stop flag. Signal handlers only ever call request_stop(),
	// which is async-signal-safe (single atomic store). A second SIGINT/SIGTERM
	// while stopping calls _exit(130): segment files stay scannable by design.
	class StopController {
	public:
		void request_stop(StopReason reason) {
			int expected = 0;
			reason_.compare_exchange_strong(expected, static_cast<int>(reason));
		}

		bool stop_requested() const { return reason_.load(std::memory_order_relaxed) != 0; }
		StopReason reason() const { return static_cast<StopReason>(reason_.load(std::memory_order_relaxed)); }

	private:
		std::atomic<int> reason_{ 0 };
	};

	const char *stop_reason_name(StopReason reason);

	// Installs SIGINT/SIGTERM handlers targeting the given controller (must
	// outlive the handlers, i.e. effectively the whole process). SIGPIPE is set
	// to ignore. Only call once, from the main thread, before spawning threads.
	void install_signal_handlers(StopController *controller);

}  // namespace jai
