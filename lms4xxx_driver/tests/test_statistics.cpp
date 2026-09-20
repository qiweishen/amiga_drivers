#include <doctest/doctest.h>

#include "statistics.h"

TEST_CASE("LiDAR scan delivery excludes telemetry and shutdown replies") {
    lms4xxx::DriverStatistics stats;
    stats.frames_received = 19343;
    stats.frames_parsed = 19328;
    stats.non_scan_frames = 15;

    const auto snapshot = stats.GetSnapshot();
    CHECK(snapshot.frames_received == 19343);
    CHECK(snapshot.non_scan_frames == 15);
    CHECK(snapshot.ScanCandidateFrames() == 19328);
    CHECK(snapshot.DeliveryRate() == doctest::Approx(100.0));
}

TEST_CASE("LiDAR scan delivery retains unclassified losses in its denominator") {
    lms4xxx::DriverStatistics stats;
    stats.frames_received = 103;
    stats.non_scan_frames = 3;
    stats.frames_parsed = 98;
    stats.parse_errors = 1;
    stats.frames_dropped = 1;

    const auto snapshot = stats.GetSnapshot();
    CHECK(snapshot.ScanCandidateFrames() == 100);
    CHECK(snapshot.DeliveryRate() == doctest::Approx(98.0));
}

TEST_CASE("LiDAR scan delivery handles an empty stream and control-only snapshots") {
    lms4xxx::DriverStatistics stats;
    CHECK(stats.GetSnapshot().DeliveryRate() == 0.0);

    stats.frames_received = 3;
    stats.non_scan_frames = 3;
    CHECK(stats.GetSnapshot().ScanCandidateFrames() == 0);
    CHECK(stats.GetSnapshot().DeliveryRate() == 0.0);

    // Relaxed live snapshots may observe a newer parser count than receive count.
    // Do not let unsigned subtraction turn this into an enormous denominator.
    stats.non_scan_frames = 4;
    CHECK(stats.GetSnapshot().ScanCandidateFrames() == 0);
    CHECK(stats.GetSnapshot().DeliveryRate() == 0.0);
}

TEST_CASE("LiDAR statistics reset non-scan accounting between acquisitions") {
    lms4xxx::DriverStatistics stats;
    stats.frames_received = 103;
    stats.frames_parsed = 100;
    stats.non_scan_frames = 3;

    stats.Reset();
    const auto snapshot = stats.GetSnapshot();
    CHECK(snapshot.frames_received == 0);
    CHECK(snapshot.frames_parsed == 0);
    CHECK(snapshot.non_scan_frames == 0);
    CHECK(snapshot.DeliveryRate() == 0.0);
}
