#include <doctest/doctest.h>

#include "recording_status.h"

TEST_CASE("A stopped LiDAR can retain every parsed scan and fail only time quality") {
    lms4xxx::DriverStatistics::Snapshot drv{};
    drv.frames_received = 19343;
    drv.frames_parsed = 19328;
    drv.device_no_ntp_events = 1;
    drv.ntp_status = lms4xxx::DriverStatistics::NtpStatus::kNotLocked;
    drv.ntp_server_reachable = true;
    lms4xxx::ScanRecordWriter::Statistics wr{};
    wr.frames_queued = wr.frames_written = 19328;

    CHECK(lms4xxx::FirstDataLoss(drv, wr) == nullptr);
    const auto result = lms4xxx::AssessRecording(drv, wr, false, false, false);
    CHECK(result.Failed());
    CHECK_FALSE(result.data_integrity_failed);
    CHECK(result.time_quality_degraded);
    REQUIRE(result.failure_reasons.size() == 1);
    CHECK(result.Summary() == "device reported No NTP signal: ntp_device_loss=1");

    // A later plausible timestamp does not erase the recorded quality event.
    drv.ntp_status = lms4xxx::DriverStatistics::NtpStatus::kUnverified;
    CHECK(lms4xxx::AssessRecording(drv, wr, false, false, false).Failed());
}

TEST_CASE("Control replies do not make a fully recorded scan pipeline incomplete") {
    lms4xxx::DriverStatistics::Snapshot drv{};
    drv.frames_received = 115;
    drv.frames_parsed = 100;
    lms4xxx::ScanRecordWriter::Statistics wr{};
    wr.frames_queued = wr.frames_written = 100;
    const auto result = lms4xxx::AssessRecording(drv, wr, false, false, false);
    CHECK_FALSE(result.Failed());
    CHECK_FALSE(result.data_integrity_failed);
    CHECK_FALSE(result.time_quality_degraded);
}

TEST_CASE("Final drain and file close failures remain failures on an operator stop") {
    lms4xxx::DriverStatistics::Snapshot drv{};
    drv.frames_parsed = 100;
    lms4xxx::ScanRecordWriter::Statistics wr{};
    wr.frames_queued = 100;
    wr.frames_written = 99;
    auto result = lms4xxx::AssessRecording(drv, wr, false, false, false);
    CHECK(result.Failed());
    CHECK(result.data_integrity_failed);
    CHECK(result.Summary().find("parsed=100 queued=100 written=99") != std::string::npos);
    wr.frames_written = 100;
    result = lms4xxx::AssessRecording(drv, wr, false, true, false);
    CHECK(result.Failed());
    CHECK(result.data_integrity_failed);
}

TEST_CASE("Transport loss and clock anomalies retain independent failure reasons") {
    lms4xxx::DriverStatistics::Snapshot drv{};
    drv.frames_dropped = 2;
    drv.utc_backwards = 1;
    drv.clock_step_events = 3;
    const auto result = lms4xxx::AssessRecording(drv, {}, true, false, true);
    CHECK(result.data_integrity_failed);
    CHECK(result.time_quality_degraded);
    REQUIRE(result.failure_reasons.size() == 4);
    CHECK(result.Summary().find("dropped_ring=2") != std::string::npos);
    CHECK(result.Summary().find("utc_back=1") != std::string::npos);
    CHECK(result.Summary().find("clock_step_events=3") != std::string::npos);
}

TEST_CASE("An earlier failure is not cleared by an otherwise clean final snapshot") {
    const auto result = lms4xxx::AssessRecording({}, {}, false, false, true);
    CHECK(result.Failed());
    CHECK_FALSE(result.failure_reasons.empty());
}

TEST_CASE("A clock still in its startup grace period is diagnosed without a new stop policy") {
    lms4xxx::DriverStatistics::Snapshot drv{};
    drv.ntp_status = lms4xxx::DriverStatistics::NtpStatus::kNotLocked;
    const auto waiting = lms4xxx::AssessRecording(drv, {}, false, false, false);
    CHECK(waiting.time_quality_degraded);
    CHECK_FALSE(waiting.Failed());

    // Once the driver's existing deadline faults, the final verdict must fail.
    const auto expired = lms4xxx::AssessRecording(drv, {}, true, false, false);
    CHECK(expired.time_quality_degraded);
    CHECK(expired.Failed());
}
