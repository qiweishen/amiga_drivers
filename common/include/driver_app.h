/// @file driver_app.h
/// @brief The unified driver-app interface every sensor driver implements.
///
/// The unified main drives all drivers through this base class: init() on the
/// main thread (in creation order), run() on a dedicated thread per driver,
/// a poll loop over TerminateFlag(), then shutdown(). Contract:
///  - init: bring the device up; return false on failure. Long bring-ups must
///    poll external_stop (and TerminateFlag()) so a shutdown request aborts.
///  - run: block until TerminateFlag() is set (externally or by an internal
///    failure); set it before returning so the whole rig shuts down together.
///  - shutdown: idempotent; safe to call after a failed or interrupted init.

#ifndef COMMON_DRIVER_APP_H
#define COMMON_DRIVER_APP_H

#include <atomic>
#include <functional>


namespace Common {
	class IDriverApp {
	public:
		IDriverApp() = default;
		virtual ~IDriverApp() = default;

		// Non-copyable, non-movable (owns threads and I/O resources)
		IDriverApp(const IDriverApp &) = delete;
		IDriverApp &operator=(const IDriverApp &) = delete;
		IDriverApp(IDriverApp &&) = delete;
		IDriverApp &operator=(IDriverApp &&) = delete;

		[[nodiscard]] virtual bool init(const std::function<bool()> &external_stop = {}) = 0;

		virtual void run() = 0;

		virtual void shutdown() = 0;

		// Shared termination flag (set by the unified main's signal handler)
		[[nodiscard]] std::atomic<bool> &TerminateFlag() { return terminate_; }

	protected:
		std::atomic<bool> terminate_{ false };
	};
}  // namespace Common

#endif	// COMMON_DRIVER_APP_H
