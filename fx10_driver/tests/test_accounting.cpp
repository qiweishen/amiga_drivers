#include "../include/accounting.hpp"

#include <gtest/gtest.h>

using fx10::BlockIdTracker;
using fx10::classify;
using fx10::Counters;
using fx10::RunStatus;

using Mode = BlockIdTracker::Mode;

TEST(BlockIdTracker, SequentialHasNoGaps) {
  BlockIdTracker tracker;
  for (std::uint64_t id = 1; id <= 5; ++id) {
    const auto obs = tracker.observe(id);
    EXPECT_EQ(obs.gap_before, 0u);
    EXPECT_FALSE(obs.anomaly);
  }
  EXPECT_EQ(tracker.observed(), 5u);
  EXPECT_EQ(tracker.totalMissed(), 0u);
}

TEST(BlockIdTracker, SimpleGap) {
  BlockIdTracker tracker;
  tracker.observe(1);
  tracker.observe(2);
  const auto obs = tracker.observe(5);  // 3 and 4 missing
  EXPECT_EQ(obs.gap_before, 2u);
  EXPECT_EQ(obs.first_missing, 3u);
  EXPECT_EQ(tracker.totalMissed(), 2u);
}

TEST(BlockIdTracker, SixteenBitWrapSkipsZero) {
  BlockIdTracker tracker;
  tracker.observe(65534);
  EXPECT_EQ(tracker.observe(65535).gap_before, 0u);
  const auto wrap = tracker.observe(1);  // ...65535, 1, 2... (0 skipped)
  EXPECT_EQ(wrap.gap_before, 0u);
  EXPECT_FALSE(wrap.anomaly);
  EXPECT_EQ(tracker.observe(2).gap_before, 0u);
  EXPECT_EQ(tracker.totalMissed(), 0u);
}

TEST(BlockIdTracker, GapAcrossWrap) {
  BlockIdTracker tracker;
  tracker.observe(65534);
  const auto obs = tracker.observe(2);  // missing 65535 and 1
  EXPECT_EQ(obs.gap_before, 2u);
  EXPECT_EQ(obs.first_missing, 65535u);
  EXPECT_FALSE(obs.anomaly);
}

TEST(BlockIdTracker, DuplicateAndBackwardsAreAnomalies) {
  BlockIdTracker tracker;
  tracker.observe(10);
  EXPECT_TRUE(tracker.observe(10).anomaly);  // duplicate
  EXPECT_TRUE(tracker.observe(5).anomaly);   // backwards
  EXPECT_EQ(tracker.anomalies(), 2u);
  EXPECT_EQ(tracker.totalMissed(), 0u);
}

TEST(BlockIdTracker, ZeroIdIsAnomalyInSixteenBitSpace) {
  BlockIdTracker tracker;
  tracker.observe(5);
  EXPECT_TRUE(tracker.observe(0).anomaly);
  // Stream continues as if 0 never happened.
  EXPECT_EQ(tracker.observe(6).gap_before, 0u);
}

TEST(BlockIdTracker, SixtyFourBitMode) {
  BlockIdTracker tracker(Mode::k64Bit);
  tracker.observe(65535);
  EXPECT_EQ(tracker.observe(65536).gap_before, 0u);  // no wrap in 64-bit mode
  EXPECT_EQ(tracker.observe(65540).gap_before, 3u);
  EXPECT_TRUE(tracker.observe(1).anomaly);  // backwards
}

TEST(BlockIdTracker, AutoStaysSixteenBitOnWrap) {
  BlockIdTracker tracker(Mode::kAuto);
  tracker.observe(65535);
  EXPECT_EQ(tracker.observe(1).gap_before, 0u);  // interpreted as 16-bit wrap
}

TEST(BlockIdTracker, AutoSwitchesToWideOnLargeId) {
  BlockIdTracker tracker(Mode::kAuto);
  tracker.observe(65535);
  const auto obs = tracker.observe(65536);  // 64-bit camera crossing the boundary
  EXPECT_EQ(obs.gap_before, 0u);
  EXPECT_FALSE(obs.anomaly);
  // Once wide, a drop back to 1 is an anomaly, not a wrap.
  EXPECT_TRUE(tracker.observe(1).anomaly);
}

TEST(Classify, CleanAndDegraded) {
  Counters c;
  EXPECT_EQ(classify(c), RunStatus::kClean);  // missed_trigger_delta = -1 (unmapped) is clean

  c.missed_trigger_delta = 0;
  EXPECT_EQ(classify(c), RunStatus::kClean);

  Counters degraded;
  degraded.frames_missed_rx = 1;
  EXPECT_EQ(classify(degraded), RunStatus::kDegraded);

  degraded = Counters{};
  degraded.op_errors = 1;
  EXPECT_EQ(classify(degraded), RunStatus::kDegraded);

  degraded = Counters{};
  degraded.write_errors = 1;
  EXPECT_EQ(classify(degraded), RunStatus::kDegraded);

  degraded = Counters{};
  degraded.blockid_anomalies = 1;
  EXPECT_EQ(classify(degraded), RunStatus::kDegraded);

  degraded = Counters{};
  degraded.missed_trigger_delta = 3;
  EXPECT_EQ(classify(degraded), RunStatus::kDegraded);

  // Padded gap lines imply frames_missed_rx > 0, but padding alone is bookkeeping.
  degraded = Counters{};
  degraded.gap_lines_padded = 5;
  EXPECT_EQ(classify(degraded), RunStatus::kClean);
}
