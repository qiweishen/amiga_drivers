/// @file common/thread_safe_queue.h
/// @brief Thread-safe bounded queue for producer-consumer patterns.
///
/// Originally from lms41xxx_driver. Moved here as a shared header-only utility.
///
/// Usage:
///   std::atomic<bool> stop{false};
///   Common::ThreadSafeQueue<MyType> queue(256, stop);
///   queue.push(item);        // returns false if full
///   queue.pop(item, 100ms); // blocks up to 100ms; returns false on timeout or stop
///   queue.try_pop(item);    // non-blocking pop

#ifndef COMMON_THREAD_SAFE_QUEUE_H
#define COMMON_THREAD_SAFE_QUEUE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>


namespace Common {
    /// Thread-safe bounded queue.
///
/// The stop_flag reference allows pop() to unblock promptly during shutdown
/// without coupling the queue to any particular global variable.
    template<typename T>
    class ThreadSafeQueue {
    public:
        ThreadSafeQueue(size_t max_size, std::atomic<bool> &stop_flag)
            : max_size_(max_size)
              , stop_flag_(stop_flag) {
        }

        ThreadSafeQueue(const ThreadSafeQueue &) = delete;

        ThreadSafeQueue &operator=(const ThreadSafeQueue &) = delete;

        /// Push an item. Returns false if the queue is full (item is not enqueued).
        bool push(T item) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.size() >= max_size_) {
                    return false;
                }
                queue_.push(std::move(item));
            }
            cv_.notify_one();
            return true;
        }

        /// Blocking pop with timeout. Returns false on timeout or if stop_flag is set.
        bool pop(T &item, std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, timeout, [this] {
                return !queue_.empty() || stop_flag_.load(std::memory_order_acquire);
            })) {
                return false;
            }
            if (queue_.empty()) {
                return false;
            }
            item = std::move(queue_.front());
            queue_.pop();
            return true;
        }

        /// Non-blocking pop. Returns false if the queue is empty.
        bool try_pop(T &item) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return false;
            }
            item = std::move(queue_.front());
            queue_.pop();
            return true;
        }

        [[nodiscard]] size_t size() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<T> queue_;
        size_t max_size_;
        std::atomic<bool> &stop_flag_;
    };
} // namespace Common


#endif // COMMON_THREAD_SAFE_QUEUE_H
