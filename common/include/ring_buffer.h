/// @file common/ring_buffer.h
/// @brief Lock-free Single-Producer Single-Consumer (SPSC) ring buffer.
///
/// Designed for the callback→writer hot path in sensor drivers (e.g., SICK LiDAR
/// at 600Hz). Replaces mutex-based ThreadSafeQueue for SPSC scenarios.
///
/// Key design decisions:
/// - Capacity rounded up to power of 2 for fast modulo via bitmask
/// - Cache-line-padded head/tail atomics to prevent false sharing
/// - Monotonically increasing indices (never wrap the atomics themselves)
/// - Placement new / explicit destructor for proper element lifetime management
/// - memory_order_acquire/release for minimum necessary synchronization
///
/// Usage:
///   Common::RingBuffer<PointCloudFrame> ring(1024);  // actual capacity = 1024
///   // Producer thread:
///   ring.try_push(std::move(frame));   // returns false if full
///   // Consumer thread:
///   PointCloudFrame frame;
///   ring.try_pop(frame);               // returns false if empty

#ifndef COMMON_RING_BUFFER_H
#define COMMON_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>


namespace Common {

/// Hardware cache line size used for padding to prevent false sharing.
/// 64 bytes is standard for x86_64 and most ARM; conservative on platforms
/// with larger lines (128 bytes on Apple M-series) but still correct.
inline constexpr std::size_t kCacheLineSize = 64;


/// Lock-free SPSC ring buffer.
///
/// Thread safety contract:
/// - Exactly ONE thread may call try_push (producer)
/// - Exactly ONE thread may call try_pop (consumer)
/// - size()/empty()/capacity() may be called from any thread (approximate)
///
/// @tparam T Element type. Must be move-constructible and move-assignable.
template<typename T>
class RingBuffer {
    static_assert(std::is_move_constructible_v<T>, "RingBuffer<T> requires T to be move-constructible");

public:
    /// Construct a ring buffer with at least `min_capacity` usable slots.
    /// Actual capacity is rounded up to the next power of 2.
    explicit RingBuffer(std::size_t min_capacity)
        : capacity_(NextPowerOf2(min_capacity < 1 ? 1 : min_capacity))
        , mask_(capacity_ - 1)
        , storage_(std::make_unique<Storage[]>(capacity_)) {
    }

    ~RingBuffer() {
        // Destroy any elements still in the buffer.
        std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_relaxed);
        while (t != h) {
            ElementAt(t)->~T();
            ++t;
        }
    }

    RingBuffer(const RingBuffer &) = delete;
    RingBuffer &operator=(const RingBuffer &) = delete;
    RingBuffer(RingBuffer &&) = delete;
    RingBuffer &operator=(RingBuffer &&) = delete;

    /// Non-blocking push by const reference (copies the item).
    /// @return true if the item was enqueued, false if the buffer is full.
    bool try_push(const T &item) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == capacity_) {
            return false; // Full.
        }
        new (ElementAt(h)) T(item);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    /// Non-blocking push by rvalue reference (moves the item).
    /// @return true if the item was enqueued, false if the buffer is full.
    bool try_push(T &&item) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == capacity_) {
            return false; // Full.
        }
        new (ElementAt(h)) T(std::move(item));
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    /// Non-blocking pop. Moves the front element into `item`.
    /// @return true if an element was dequeued, false if the buffer is empty.
    bool try_pop(T &item) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) {
            return false; // Empty.
        }
        T *elem = ElementAt(t);
        item = std::move(*elem);
        elem->~T();
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    /// Approximate number of elements currently in the buffer.
    /// Safe to call from any thread, but may be stale by the time the caller
    /// acts on it. Intended for monitoring/logging, not for flow control.
    [[nodiscard]] std::size_t size() const {
        // Acquire both to get a consistent-ish snapshot.
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return h - t;
    }

    /// Maximum number of usable slots.
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

    /// True if the buffer appears empty (approximate, see size()).
    [[nodiscard]] bool empty() const { return size() == 0; }

private:
    /// Round up to the next power of 2 (returns n if already a power of 2).
    static std::size_t NextPowerOf2(std::size_t n) {
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        if constexpr (sizeof(std::size_t) >= 8) {
            n |= n >> 32;
        }
        return n + 1;
    }

    /// Access the element at a given monotonic index.
    T *ElementAt(std::size_t index) {
        return reinterpret_cast<T *>(&storage_[index & mask_]);
    }

    // --- Layout: separate cache lines for producer and consumer ---

    // Immutable after construction (shared by both threads, no contention).
    const std::size_t capacity_;
    const std::size_t mask_;

    // Aligned storage for elements (avoids default-constructing T).
    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
    std::unique_ptr<Storage[]> storage_;

    // Producer writes head_, consumer reads it.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};

    // Consumer writes tail_, producer reads it.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
};

} // namespace Common


#endif // COMMON_RING_BUFFER_H
