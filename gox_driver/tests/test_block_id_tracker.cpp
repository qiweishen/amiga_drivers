// BlockID gap accounting (src/block_id_tracker.cpp). With
// GevGVSPExtendedIDMode forced On the IDs are 64-bit, so a gap is always a
// genuine loss and never a wrap.

#include "block_id_tracker.h"

#include <doctest/doctest.h>

TEST_CASE("block_id_tracker: the first frame can never be a gap") {
    gox::BlockIdTracker t;
    CHECK_FALSE(t.HasLast());
    const gox::BlockIdGap g = t.Observe(5000); // a session can start at any ID
    CHECK_FALSE(g.gap);
    CHECK(g.missing == 0u);
    CHECK(t.HasLast());
    CHECK(t.Last() == 5000u);
}

TEST_CASE("block_id_tracker: consecutive IDs report no gap") {
    gox::BlockIdTracker t;
    t.Observe(1);
    for (uint64_t id = 2; id <= 100; ++id) {
        const gox::BlockIdGap g = t.Observe(id);
        CAPTURE(id);
        CHECK_FALSE(g.gap);
        CHECK(g.missing == 0u);
    }
    CHECK(t.Last() == 100u);
}

TEST_CASE("block_id_tracker: one and many missing IDs are counted exactly") {
    gox::BlockIdTracker t;
    t.Observe(10);

    const gox::BlockIdGap one = t.Observe(12); // 11 never arrived
    CHECK(one.gap);
    CHECK(one.missing == 1u);

    const gox::BlockIdGap many = t.Observe(1012); // 13..1011 never arrived
    CHECK(many.gap);
    CHECK(many.missing == 999u);

    const gox::BlockIdGap next = t.Observe(1013); // the gap does not linger
    CHECK_FALSE(next.gap);
}

TEST_CASE("block_id_tracker: a dropped frame must still be observed") {
    // The acquisition loop drops degraded frames under on_buffer_error=drop.
    // Observing every arrival is what keeps the next frame from reporting a gap
    // that never happened (and double-counting it in the emitted-frame rate).
    gox::BlockIdTracker t;
    t.Observe(1);
    t.Observe(2); // arrived, dropped afterwards
    const gox::BlockIdGap g = t.Observe(3);
    CHECK_FALSE(g.gap);
    CHECK(g.missing == 0u);
}

TEST_CASE("block_id_tracker: a repeated or out-of-order ID is not a gap") {
    gox::BlockIdTracker t;
    t.Observe(10);
    CHECK_FALSE(t.Observe(10).gap); // same ID again
    CHECK_FALSE(t.Observe(9).gap); // older ID
    CHECK(t.Last() == 9u);
}

TEST_CASE("block_id_tracker: 64-bit IDs are handled without overflow") {
    gox::BlockIdTracker t;
    const uint64_t big = 0xFFFFFFFFFFFFFF00ull;
    t.Observe(big);
    const gox::BlockIdGap g = t.Observe(big + 3);
    CHECK(g.gap);
    CHECK(g.missing == 2u);
}
