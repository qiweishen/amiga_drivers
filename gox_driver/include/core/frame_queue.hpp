#pragma once

// Bounded MPSC-safe queue (used as SPSC: one acquisition thread pushes, one
// writer thread pops). mutex+condvar is comfortably fast enough at GigE
// frame rates (tens to hundreds of frames per second).

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace jai {

	template<typename T>
	class BoundedQueue {
	public:
		explicit BoundedQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

		// Non-blocking. Returns false (and drops nothing; caller keeps the item)
		// when the queue is full or closed.
		bool try_push(T &&item) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (closed_ || items_.size() >= capacity_) {
					return false;
				}
				items_.push_back(std::move(item));
			}
			not_empty_.notify_one();
			return true;
		}

		// Blocks up to `timeout` while full. Returns false without consuming the
		// item when the queue is still full — or closed — at timeout, so the
		// caller can re-check its stop flags and retry with the same item.
		template<typename Rep, typename Period>
		bool push_wait_for(T &&item, const std::chrono::duration<Rep, Period> &timeout) {
			{
				std::unique_lock<std::mutex> lock(mutex_);
				if (!not_full_.wait_for(lock, timeout, [&] { return closed_ || items_.size() < capacity_; })) {
					return false;
				}
				if (closed_) {
					return false;
				}
				items_.push_back(std::move(item));
			}
			not_empty_.notify_one();
			return true;
		}

		// Blocks while full. Returns false only if the queue is closed.
		bool push_blocking(T &&item) {
			{
				std::unique_lock<std::mutex> lock(mutex_);
				not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
				if (closed_) {
					return false;
				}
				items_.push_back(std::move(item));
			}
			not_empty_.notify_one();
			return true;
		}

		// Blocks until an item is available or the queue is closed and drained.
		// Returns false on closed-and-empty.
		bool pop(T &out) {
			std::unique_lock<std::mutex> lock(mutex_);
			not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
			if (items_.empty()) {
				return false;  // closed and drained
			}
			out = std::move(items_.front());
			items_.pop_front();
			lock.unlock();
			not_full_.notify_one();
			return true;
		}

		// After close(), pushes fail and pop() drains the remaining items then
		// returns false. Idempotent.
		void close() {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				closed_ = true;
			}
			not_empty_.notify_all();
			not_full_.notify_all();
		}

		bool closed() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return closed_;
		}

		size_t size() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return items_.size();
		}

		size_t capacity() const { return capacity_; }

	private:
		const size_t capacity_;
		mutable std::mutex mutex_;
		std::condition_variable not_empty_;
		std::condition_variable not_full_;
		std::deque<T> items_;
		bool closed_ = false;
	};

}  // namespace jai
