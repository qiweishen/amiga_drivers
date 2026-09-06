// Buffer pool sizing rules (src/buffering.cpp). A GOX-12405C frame is ~18.6 MB,
// so every buffer counted here is real memory.

#include "buffering.h"

#include <doctest/doctest.h>

TEST_CASE("buffering: the GVSP buffer count scales with the rate inside its clamps") {
    // ~1 s of frames plus 8 of headroom, clamped to [8, 64].
    CHECK(gox::AutoBufferCount(3.0, 0) == 11u); // ceil(3) + 8
    CHECK(gox::AutoBufferCount(24.0, 0) == 32u); // ceil(24) + 8
    CHECK(gox::AutoBufferCount(0.5, 0) == 9u); // ceil(0.5) + 8

    // A very high configured rate must not turn into gigabytes of buffers.
    CHECK(gox::AutoBufferCount(120.0, 0) == 64u);
    CHECK(gox::AutoBufferCount(100000.0, 0) == 64u);

    // No rate configured (external trigger, or an omitted key): a fixed default.
    CHECK(gox::AutoBufferCount(0.0, 0) == 16u);
    CHECK(gox::AutoBufferCount(-1.0, 0) == 16u);
}

TEST_CASE("buffering: the stream's own queue ceiling wins") {
    // Queueing past PvStream::GetQueuedBufferMaximum() fails the bring-up with
    // an opaque SDK error, so the rule clamps to it.
    CHECK(gox::AutoBufferCount(24.0, 10) == 10u);
    CHECK(gox::AutoBufferCount(3.0, 4) == 4u);
    CHECK(gox::AutoBufferCount(3.0, 1) == 1u);
    // 0 means "unknown": no clamping.
    CHECK(gox::AutoBufferCount(3.0, 0) == 11u);
    // A ceiling above the computed value changes nothing.
    CHECK(gox::AutoBufferCount(3.0, 256) == 11u);
    // Never zero, whatever the inputs.
    CHECK(gox::AutoBufferCount(0.0, 0) >= 1u);
}

TEST_CASE("buffering: the frame queue absorbs about two seconds of writer stall") {
    CHECK(gox::AutoQueueFrames(3.0) == 6u);
    CHECK(gox::AutoQueueFrames(10.0) == 20u);
    // Floor: a queue of one or two frames degrades "block" into stalling.
    CHECK(gox::AutoQueueFrames(0.125) == 4u);
    CHECK(gox::AutoQueueFrames(1.0) == 4u);
    // Ceiling: at 18.6 MB a frame, 64 is already 1.2 GB.
    CHECK(gox::AutoQueueFrames(1000.0) == 64u);
    // No rate configured.
    CHECK(gox::AutoQueueFrames(0.0) == 8u);
}
