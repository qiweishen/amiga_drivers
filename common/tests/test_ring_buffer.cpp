#include <doctest/doctest.h>

#include "ring_buffer.h"

TEST_CASE("RingBuffer: capacity rounds up to a power of two and the queue is FIFO") {
    common::RingBuffer<int> ring(5);
    CHECK(ring.capacity() >= 5u);
    CHECK((ring.capacity() & (ring.capacity() - 1)) == 0u);
    CHECK(ring.empty());

    std::size_t pushed = 0;
    while (ring.try_push(static_cast<int>(pushed))) {
        ++pushed;
    }
    CHECK(pushed >= 5u);
    CHECK_FALSE(ring.try_push(99)); // full

    int v = -1;
    for (std::size_t i = 0; i < pushed; ++i) {
        REQUIRE(ring.try_pop(v));
        CHECK(v == static_cast<int>(i));
    }
    CHECK_FALSE(ring.try_pop(v));
    CHECK(ring.empty());
}
