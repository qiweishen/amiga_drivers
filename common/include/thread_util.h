/// @file thread_util.h
/// @brief Thread scheduling helpers and the shared terminate-wait loop
/// (header-only). Scheduling calls return 0 or an errno value so the caller
/// decides how to log — inside containers without CAP_SYS_NICE they are
/// expected to fail and callers should degrade with a warning only.

#ifndef COMMON_THREAD_UTIL_H
#define COMMON_THREAD_UTIL_H

#include <atomic>
#include <chrono>
#include <pthread.h>
#include <sched.h>
#include <thread>


namespace Common::ThreadUtil {
	// SCHED_FIFO with the given priority. Returns 0 on success, else errno
	[[nodiscard]] inline int SetRealtimePriority(std::thread &t, int priority) {
		sched_param param{};
		param.sched_priority = priority;
		return pthread_setschedparam(t.native_handle(), SCHED_FIFO, &param);
	}

	// Pin the thread to one CPU core. Returns 0 on success, else errno
	[[nodiscard]] inline int PinToCpu(std::thread &t, int cpu) {
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(cpu, &cpuset);
		return pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
	}

	// The shared driver run() idle loop: block until the terminate flag is set
	inline void WaitUntilTerminated(const std::atomic<bool> &terminate,
									std::chrono::milliseconds poll = std::chrono::milliseconds(100)) {
		while (!terminate.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(poll);
		}
	}
}  // namespace Common::ThreadUtil

#endif	// COMMON_THREAD_UTIL_H
