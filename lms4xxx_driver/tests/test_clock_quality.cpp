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

TEST_CASE("LiDAR recorded 19 ms UTC regression is visible below the old 1000 ms fault limit") {
    lms4xxx::ClockQualityTracker tracker;
    tracker.Observe(1789103434333000LL, true, 100000, 1000);
    const auto observation = tracker.Observe(1789103434314000LL, true, 101644, 1000);
    CHECK(observation.step_us == -20644);
    CHECK((observation.flags & lms4xxx::ClockFlag::kBackwardUtc) != 0);
    CHECK((observation.flags & lms4xxx::ClockFlag::kStepExceeded) == 0);
    CHECK((observation.flags & lms4xxx::ClockFlag::kAbsoluteTimeUnverified) != 0);
}

TEST_CASE("LiDAR distinguishes timestamp quantization, uptime rollover and reset") {
    lms4xxx::ClockQualityTracker tracker;
    const auto first = tracker.Observe(1000000, true, 0xffffff00u, 1000);
    CHECK((first.flags & lms4xxx::ClockFlag::kFirstSample) != 0);
    const auto rollover = tracker.Observe(1001000, true, 744, 1000);
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
