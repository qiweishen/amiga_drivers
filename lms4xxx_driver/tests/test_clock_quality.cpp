#include <doctest/doctest.h>
#include "clock_quality.h"
#include "scan_record.h"
#include "cola_b.h"

TEST_CASE("LiDAR STOP acknowledgement is a control reply, not a malformed scan") {
    CHECK_FALSE(lms4xxx::IsScanMessage({"sEA", "LMDscandata", {0}}));
    CHECK_FALSE(lms4xxx::IsScanMessage({"sEA", "LMDscandata", {1}}));
    CHECK(lms4xxx::IsScanMessage({"sSN", "LMDscandata", {}}));
    CHECK(lms4xxx::IsScanMessage({"sRA", "LMDscandata", {}}));
    CHECK_FALSE(lms4xxx::IsScanMessage({"sRA", "SCdevicestate", {1}}));
}

TEST_CASE("LiDAR recorded 19 ms UTC regression is anomalous below the 1000 ms fault limit") {
    lms4xxx::ClockQualityTracker tracker;
    tracker.Observe(1789103434333000LL, true, 100000, 1000);
    const auto observation = tracker.Observe(1789103434314000LL, true, 101644, 1000);
    CHECK(observation.utc_delta_us == -19000);
    CHECK(observation.uptime_delta_us == 1644);
    CHECK(observation.step_us == -20644);
    CHECK((observation.flags & lms4xxx::ClockFlag::kBackwardUtc) != 0);
    CHECK((observation.flags & lms4xxx::ClockFlag::kStepExceeded) == 0);
    CHECK((observation.flags & lms4xxx::ClockFlag::kAbsoluteTimeUnverified) != 0);
    CHECK(lms4xxx::IsTimeAnomaly(observation));
}

TEST_CASE("LiDAR UTC regression anomaly requires strictly more than 1 ms") {
    for (const std::int64_t delta : {-1001, -1000, -999, 0, 1000}) {
        CAPTURE(delta);
        lms4xxx::ClockQualityTracker tracker;
        tracker.Observe(1000000, true, 100000, 1000);
        const auto observation = tracker.Observe(1000000 + delta, true, 101600, 1000);
        CHECK(observation.utc_delta_us == delta);
        CHECK(lms4xxx::IsTimeAnomaly(observation) == (delta < -1000));
        CHECK((observation.flags & lms4xxx::ClockFlag::kStepExceeded) == 0);
        if (delta < 0) {
            CHECK((observation.flags & lms4xxx::ClockFlag::kBackwardUtc) != 0);
            CHECK(lms4xxx::ShouldLogClockChange(observation));
        }
    }
}

TEST_CASE("LiDAR UTC uptime anomaly requires strictly more than 10 ms in either direction") {
    for (const std::int64_t step : {-10001, -10000, -9999, 0, 9999, 10000, 10001}) {
        CAPTURE(step);
        lms4xxx::ClockQualityTracker tracker;
        tracker.Observe(1000000, true, 100000, 1000);
        const auto observation = tracker.Observe(1020000 + step, true, 120000, 1000);
        CHECK(observation.utc_delta_us > 0); // isolate the relative-step condition
        CHECK(observation.step_us == step);
        CHECK(lms4xxx::IsTimeAnomaly(observation) == (step < -10000 || step > 10000));
        CHECK((observation.flags & lms4xxx::ClockFlag::kStepExceeded) == 0);
    }
}

TEST_CASE("LiDAR a 1 ms UTC regression can still exceed the relative-step anomaly limit") {
    lms4xxx::ClockQualityTracker tracker;
    tracker.Observe(1000000, true, 100000, 1000);
    const auto observation = tracker.Observe(999000, true, 109001, 1000);
    CHECK(observation.utc_delta_us == -1000);
    CHECK(observation.step_us == -10001);
    CHECK(lms4xxx::IsTimeAnomaly(observation));
}

TEST_CASE("LiDAR anomaly assessment excludes unavailable deltas but retains actual UTC regression") {
    lms4xxx::ClockQualityTracker tracker;
    CHECK_FALSE(lms4xxx::IsTimeAnomaly(tracker.Observe(1000000, true, 100000, 1000)));
    const auto reset = tracker.Observe(1100000, true, 10, 1000);
    CHECK((reset.flags & lms4xxx::ClockFlag::kUptimeDiscontinuity) != 0);
    CHECK_FALSE(lms4xxx::IsTimeAnomaly(reset));
    CHECK_FALSE(lms4xxx::IsTimeAnomaly(tracker.Observe(1099000, true, 10, 1000)));
    CHECK(lms4xxx::IsTimeAnomaly(tracker.Observe(1097999, true, 10, 1000)));
    CHECK_FALSE(lms4xxx::IsTimeAnomaly(tracker.Observe(0, false, 100, 1000)));
    CHECK_FALSE(lms4xxx::IsTimeAnomaly(tracker.Observe(2000000, true, 1000, 1000)));
    CHECK_FALSE(lms4xxx::IsTimeAnomaly({}));
    CHECK_FALSE(lms4xxx::IsTimeAnomaly({lms4xxx::ClockFlag::kAssessed | lms4xxx::ClockFlag::kNtpDisabled, 0}));
}

TEST_CASE("LiDAR distinguishes timestamp quantization, uptime rollover and reset") {
    lms4xxx::ClockQualityTracker tracker;
    const auto first = tracker.Observe(1000000, true, 0xffffff00u, 1000);
    CHECK((first.flags & lms4xxx::ClockFlag::kFirstSample) != 0);
    const auto rollover = tracker.Observe(1001000, true, 744, 1000);
    CHECK(rollover.utc_delta_us == 1000);
    CHECK(rollover.uptime_delta_us == 1000);
    CHECK(rollover.step_us == 0);
    CHECK((rollover.flags & lms4xxx::ClockFlag::kUptimeDiscontinuity) == 0);
    const auto repeated = tracker.Observe(1001000, true, 1744, 1000);
    CHECK((repeated.flags & lms4xxx::ClockFlag::kRepeatedUtc) != 0);
    CHECK((repeated.flags & lms4xxx::ClockFlag::kBackwardUtc) == 0);
    const auto reset = tracker.Observe(1002000, true, 10, 1000);
    CHECK((reset.flags & lms4xxx::ClockFlag::kUptimeDiscontinuity) != 0);
    CHECK((tracker.Observe(0, false, 100, 1000).flags & lms4xxx::ClockFlag::kInvalidTimestamp) != 0);
    CHECK((tracker.Observe(2000000, true, 1000, 1000).flags & lms4xxx::ClockFlag::kFirstSample) != 0);
}

TEST_CASE("LiDAR UTC regression offset survives an unusable uptime delta") {
    lms4xxx::ClockQualityTracker tracker;
    tracker.Observe(1789103434333000LL, true, 100000, 1000);
    const auto observation = tracker.Observe(1789103434314000LL, true, 100000, 1000);
    CHECK(observation.utc_delta_us == -19000);
    CHECK(observation.uptime_delta_us == 0);
    CHECK((observation.flags & lms4xxx::ClockFlag::kBackwardUtc) != 0);
    CHECK((observation.flags & lms4xxx::ClockFlag::kUptimeDiscontinuity) != 0);

    const auto invalid = tracker.Observe(0, false, 100100, 1000);
    CHECK(invalid.utc_delta_us == 0);
    const auto fresh = tracker.Observe(1789103434343000LL, true, 100200, 1000);
    CHECK((fresh.flags & lms4xxx::ClockFlag::kFirstSample) != 0);
    CHECK(fresh.utc_delta_us == 0); // no fabricated offset across an invalid sample
    CHECK(fresh.uptime_delta_us == 0);
}

TEST_CASE("LiDAR logs each UTC regression regardless of its size or earlier events") {
    lms4xxx::ClockQualityTracker tracker;
    CHECK_FALSE(lms4xxx::ShouldLogClockChange(tracker.Observe(1000000, true, 100000, 1000)));
    for (std::uint32_t i = 1; i <= 2; ++i) {
        const auto backward = tracker.Observe(1000000 - i, true, 100000 + i * 100, 1000);
        CHECK(backward.utc_delta_us == -1);
        CHECK(backward.step_us == -101); // below the independent 1000 us logging threshold
        CHECK(lms4xxx::ShouldLogClockChange(backward));
        CHECK((backward.flags & lms4xxx::ClockFlag::kStepExceeded) == 0);
    }
}

TEST_CASE("LiDAR clock disagreement logging includes both exact 1 ms boundaries") {
    for (const std::int64_t step : {-1001, -1000, -999, 0, 999, 1000, 1001}) {
        CAPTURE(step);
        lms4xxx::ClockQualityTracker tracker;
        tracker.Observe(1000000, true, 100000, 1000);
        const auto observation = tracker.Observe(1002000 + step, true, 102000, 1000);
        CHECK(observation.utc_delta_us > 0); // even a negative step can have increasing UTC
        CHECK(observation.step_us == step);
        CHECK(lms4xxx::ShouldLogClockChange(observation) == (step <= -1000 || step >= 1000));
        CHECK((observation.flags & lms4xxx::ClockFlag::kStepExceeded) == 0); // no new stop policy
    }
}

TEST_CASE("LiDAR log policy handles repeated UTC, invalid samples and unusable uptime") {
    lms4xxx::ClockQualityTracker tracker;
    tracker.Observe(1000000, true, 100000, 1000);
    const auto repeated = tracker.Observe(1000000, true, 101000, 1000);
    CHECK(repeated.step_us == -1000);
    CHECK(lms4xxx::ShouldLogClockChange(repeated));
    const auto uptime_reset = tracker.Observe(1010000, true, 10, 1000);
    CHECK((uptime_reset.flags & lms4xxx::ClockFlag::kUptimeDiscontinuity) != 0);
    CHECK_FALSE(lms4xxx::ShouldLogClockChange(uptime_reset));
    const auto backward_with_stalled_uptime = tracker.Observe(1009999, true, 10, 1000);
    CHECK((backward_with_stalled_uptime.flags & lms4xxx::ClockFlag::kUptimeDiscontinuity) != 0);
    CHECK(lms4xxx::ShouldLogClockChange(backward_with_stalled_uptime));
    CHECK_FALSE(lms4xxx::ShouldLogClockChange(tracker.Observe(0, false, 100, 1000)));
    CHECK_FALSE(lms4xxx::ShouldLogClockChange(tracker.Observe(2000000, true, 1000, 1000)));
    CHECK_FALSE(lms4xxx::ShouldLogClockChange({}));
    CHECK_FALSE(lms4xxx::ShouldLogClockChange({lms4xxx::ClockFlag::kAssessed | lms4xxx::ClockFlag::kNtpDisabled, 0}));
}

TEST_CASE("LiDAR quality metadata accompanies unmodified scan data into the writer record") {
    lms4xxx::ScanData scan;
    scan.time_since_startup_us = 101644;
    scan.host_receive_monotonic_us = 900000;
    scan.clock_observation = {lms4xxx::ClockFlag::kAssessed | lms4xxx::ClockFlag::kBackwardUtc, -20644};
    lms4xxx::ScanRecord record{};
    lms4xxx::FillScanRecord(scan, 0, record);
    CHECK(record.meta.time_since_startup_us == 101644);
    CHECK(record.meta.host_receive_monotonic_us == 900000);
    CHECK(record.meta.clock_step_us == -20644);
    CHECK(record.meta.clock_quality_flags == scan.clock_observation.flags);
}
