// Tests for BoundedQueue (core/frame_queue.hpp): capacity enforcement,
// blocking behavior, close/drain semantics and a producer/consumer ordering
// stress run. Worker threads only record results; assertions run on the test
// thread after join() so a failing REQUIRE can never unwind a worker.

#include "core/frame_queue.hpp"

#include "core/frame.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

TEST_CASE("frame_queue: try_push respects capacity and close") {
    jai::BoundedQueue<int> q(2);
    CHECK(q.capacity() == 2u);
    CHECK(q.size() == 0u);
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK_FALSE(q.try_push(3)); // full: rejected, nothing dropped
    CHECK(q.size() == 2u);

    int v = 0;
    CHECK(q.pop(v));
    CHECK(v == 1); // FIFO order
    CHECK(q.try_push(3)); // space again after the pop

    q.close();
    CHECK(q.closed());
    CHECK_FALSE(q.try_push(4)); // closed: rejected
    // close() drains: remaining items still come out in order, then false.
    CHECK(q.pop(v));
    CHECK(v == 2);
    CHECK(q.pop(v));
    CHECK(v == 3);
    CHECK_FALSE(q.pop(v)); // closed and drained
}

TEST_CASE("frame_queue: zero capacity is clamped to one") {
    jai::BoundedQueue<int> q(0);
    CHECK(q.capacity() == 1u);
    CHECK(q.try_push(7));
    CHECK_FALSE(q.try_push(8));
}

TEST_CASE("frame_queue: push_blocking blocks while full and unblocks on pop") {
    jai::BoundedQueue<int> q(1);
    REQUIRE(q.try_push(10));

    std::atomic<bool> pushed{false};
    bool push_result = false;
    std::thread t([&] {
        push_result = q.push_blocking(11); // must block: queue is full
        pushed.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(pushed.load()); // still parked on the full queue

    int v = 0;
    REQUIRE(q.pop(v)); // makes room; wakes the producer
    CHECK(v == 10);
    t.join();
    CHECK(pushed.load());
    CHECK(push_result);
    REQUIRE(q.pop(v));
    CHECK(v == 11);
}

TEST_CASE("frame_queue: close wakes a blocked pop") {
    jai::BoundedQueue<int> q(4);
    bool pop_result = true;
    std::thread t([&] {
        int v = 0;
        pop_result = q.pop(v); // must block: queue is empty
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    t.join();
    CHECK_FALSE(pop_result); // woken with "closed and drained"
    CHECK(q.closed());
}

TEST_CASE("frame_queue: close wakes a blocked push_blocking and keeps items drainable") {
    jai::BoundedQueue<int> q(1);
    REQUIRE(q.try_push(1));
    bool push_result = true;
    std::thread t([&] { push_result = q.push_blocking(2); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    t.join();
    CHECK_FALSE(push_result); // rejected by close, item stays with the caller

    int v = 0;
    CHECK(q.pop(v)); // the item that made it in is still drainable
    CHECK(v == 1);
    CHECK_FALSE(q.pop(v));
}

TEST_CASE("frame_queue: producer/consumer stress preserves order, count and capacity bound") {
    constexpr int kCount = 100000; // 1e5 items through a small queue
    constexpr size_t kCapacity = 32;
    jai::BoundedQueue<int> q(kCapacity);

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            if (!q.push_blocking(int(i))) {
                return; // queue closed underneath us; the count check below will fail
            }
        }
        q.close(); // EOS
    });

    std::vector<int> got;
    got.reserve(kCount);
    size_t max_size = 0;
    int v = 0;
    while (q.pop(v)) {
        got.push_back(v);
        max_size = std::max(max_size, q.size()); // snapshot under the queue's own lock
    }
    producer.join();

    REQUIRE(got.size() == static_cast<size_t>(kCount));
    bool in_order = true;
    for (int i = 0; i < kCount; ++i) {
        if (got[static_cast<size_t>(i)] != i) {
            in_order = false;
            break;
        }
    }
    CHECK(in_order);              // exact sequence: nothing lost, duplicated or reordered
    CHECK(max_size <= kCapacity); // size() never exceeded the configured capacity
}

TEST_CASE("frame_queue: FrameChunkPtr moves through without copies") {
    jai::BoundedQueue<jai::FrameChunkPtr> q(2);
    jai::FrameChunkPtr chunk = std::make_unique<jai::FrameChunk>(64);
    chunk->meta.block_id = 7;
    chunk->data[0] = 0xAB;
    const jai::FrameChunk* raw = chunk.get();

    REQUIRE(q.try_push(std::move(chunk)));
    CHECK(chunk == nullptr); // ownership transferred on a successful push

    jai::FrameChunkPtr out;
    REQUIRE(q.pop(out));
    REQUIRE(out != nullptr);
    CHECK(out.get() == raw); // the very same object came out: no copy
    CHECK(out->meta.block_id == 7u);
    CHECK(out->data[0] == 0xAB);
    CHECK(out->capacity == 64u);

    // A failed try_push must leave ownership with the caller (drop_newest
    // relies on this to return the chunk to the pool).
    REQUIRE(q.try_push(std::make_unique<jai::FrameChunk>(8)));
    REQUIRE(q.try_push(std::make_unique<jai::FrameChunk>(8)));
    CHECK_FALSE(q.try_push(std::move(out)));
    CHECK(out != nullptr);
    CHECK(out.get() == raw);
    q.close();
    // Pending chunks are unique_ptr-owned and destroyed with the queue.
}
