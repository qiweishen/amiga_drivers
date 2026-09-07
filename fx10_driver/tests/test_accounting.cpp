#include "../include/accounting.h"

#include <doctest/doctest.h>
#include <initializer_list>

using fx10::BlockIdTracker;
using fx10::Classify;
using fx10::Counters;
using fx10::RunStatus;

using Mode = BlockIdTracker::Mode;

TEST_CASE("BlockIdTracker: SequentialHasNoGaps") {
    BlockIdTracker tracker;
    for (std::uint64_t id = 1; id <= 5; ++id) {
        const auto obs = tracker.Observe(id);
        CHECK(obs.gap_before == 0u);
        CHECK_FALSE(obs.anomaly);
    }
    CHECK(tracker.Observed() == 5u);
    CHECK(tracker.TotalMissed() == 0u);
}

TEST_CASE("BlockIdTracker: SimpleGap") {
    BlockIdTracker tracker;
    tracker.Observe(1);
    tracker.Observe(2);
    const auto obs = tracker.Observe(5); // 3 and 4 missing
    CHECK(obs.gap_before == 2u);
    CHECK(obs.first_missing == 3u);
    CHECK(tracker.TotalMissed() == 2u);
}

TEST_CASE("BlockIdTracker: SixteenBitWrapSkipsZero") {
    BlockIdTracker tracker;
    tracker.Observe(65534);
    CHECK(tracker.Observe(65535).gap_before == 0u);
    const auto wrap = tracker.Observe(1); // ...65535, 1, 2... (0 skipped)
    CHECK(wrap.gap_before == 0u);
    CHECK_FALSE(wrap.anomaly);
    CHECK(tracker.Observe(2).gap_before == 0u);
    CHECK(tracker.TotalMissed() == 0u);
}

TEST_CASE("BlockIdTracker: GapAcrossWrap") {
    BlockIdTracker tracker;
    tracker.Observe(65534);
    const auto obs = tracker.Observe(2); // missing 65535 and 1
    CHECK(obs.gap_before == 2u);
    CHECK(obs.first_missing == 65535u);
    CHECK_FALSE(obs.anomaly);
}

TEST_CASE("BlockIdTracker: DuplicateAndBackwardsAreAnomalies") {
    BlockIdTracker tracker;
    tracker.Observe(10);
    CHECK(tracker.Observe(10).anomaly); // duplicate
    CHECK(tracker.Observe(5).anomaly); // backwards
    CHECK(tracker.Anomalies() == 2u);
    CHECK(tracker.TotalMissed() == 0u);
}

TEST_CASE("BlockIdTracker: ZeroIdIsAnomalyInSixteenBitSpace") {
    BlockIdTracker tracker;
    tracker.Observe(5);
    CHECK(tracker.Observe(0).anomaly);
    // Stream continues as if 0 never happened.
    CHECK(tracker.Observe(6).gap_before == 0u);
}

TEST_CASE("BlockIdTracker: ReorderingDoesNotCreateFalseLossOnRecovery") {
    for (const Mode mode : {Mode::k16Bit, Mode::k64Bit}) {
        BlockIdTracker tracker(mode);
        CHECK(tracker.Observe(0).anomaly);
        CHECK_FALSE(tracker.Observe(100).anomaly);
        CHECK(tracker.Observe(99).anomaly);
        CHECK(tracker.Observe(100).anomaly);
        CHECK(tracker.Observe(101).gap_before == 0);
        CHECK(tracker.Observe(103).gap_before == 1);
        CHECK(tracker.Observe(104).gap_before == 0);
        CHECK(tracker.TotalMissed() == 1);
    }
}

TEST_CASE("BlockIdTracker: SixtyFourBitMode") {
    BlockIdTracker tracker(Mode::k64Bit);
    tracker.Observe(65535);
    CHECK(tracker.Observe(65536).gap_before == 0u); // no wrap in 64-bit mode
    CHECK(tracker.Observe(65540).gap_before == 3u);
    CHECK(tracker.Observe(1).anomaly); // backwards
}

TEST_CASE("BlockIdTracker: AutoStaysSixteenBitOnWrap") {
    BlockIdTracker tracker(Mode::kAuto);
    tracker.Observe(65535);
    CHECK(tracker.Observe(1).gap_before == 0u); // interpreted as 16-bit wrap
}

TEST_CASE("BlockIdTracker: AutoSwitchesToWideOnLargeId") {
    BlockIdTracker tracker(Mode::kAuto);
    tracker.Observe(65535);
    const auto obs = tracker.Observe(65536); // 64-bit camera crossing the boundary
    CHECK(obs.gap_before == 0u);
    CHECK_FALSE(obs.anomaly);
    // Once wide, a drop back to 1 is an anomaly, not a wrap.
    CHECK(tracker.Observe(1).anomaly);
}

TEST_CASE("Classify: CleanAndDegraded") {
    Counters c;
    CHECK(Classify(c) == RunStatus::kClean); // missed_trigger_delta = -1 (unmapped) is clean

    c.missed_trigger_delta = 0;
    CHECK(Classify(c) == RunStatus::kClean);

    Counters degraded;
    degraded.frames_missed_rx = 1;
    CHECK(Classify(degraded) == RunStatus::kDegraded);

    degraded = Counters{};
    degraded.op_errors = 1;
    CHECK(Classify(degraded) == RunStatus::kDegraded);

    degraded = Counters{};
    degraded.write_errors = 1;
    CHECK(Classify(degraded) == RunStatus::kDegraded);

    degraded = Counters{};
    degraded.blockid_anomalies = 1;
    CHECK(Classify(degraded) == RunStatus::kDegraded);

    degraded = Counters{};
    degraded.missed_trigger_delta = 3;
    CHECK(Classify(degraded) == RunStatus::kDegraded);

    // Padded gap lines imply frames_missed_rx > 0, but padding alone is bookkeeping.
    degraded = Counters{};
    degraded.gap_lines_padded = 5;
    CHECK(Classify(degraded) == RunStatus::kClean);
}
