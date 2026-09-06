#include "../include/log_growth_tracker.h"
#include "../../submodule/sensor_trigger/session_protocol.h"

#include <doctest/doctest.h>

#include <chrono>

// The timing log is the only time source for recorded lines, so "the log has
// stopped growing" aborts a session (fx10_driver_app.cpp). The decision used to
// compare st_mtime against the WALL clock: 1 s granularity, and any NTP step —
// routine on a rig that has just acquired GNSS time — fabricated a stall and
// killed a healthy recording.

using fx10::LogGrowthTracker;
using Clock = LogGrowthTracker::Clock;

TEST_CASE("SensorSyncProtocol: StartStopErrorsRestartsAndLossAreDistinguished") {
    SensorSyncProtocol p;
    p.observe("#SESSION,START,run", false);
    p.observe("#H up=100 en=1 pps=3 ppsdrops=0 todtrunc=0 tdrops=0 sdrops=0 host=0", false);
    CHECK_FALSE(p.failed);
    CHECK(p.starts == 1);
    p.observe("#HFINAL ppsdrops=0 todtrunc=0 tdrops=0 sdrops=0 host=0 skip=0 stall=0", true);
    p.observe("#SESSION,STOP,run", true);
    p.observe("#IDLE,up=200", true);
    CHECK(p.stop_received);
    CHECK_FALSE(p.failed);
    SensorSyncProtocol old_firmware;
    old_firmware.observe("#SESSION,START,run", false);
    old_firmware.observe("#SESSION,STOP,run", true);
    CHECK(old_firmware.failed); // no final loss counters: incomplete audit, never a clean stop
    for (const char *line : {"#ERR,bad_freq,0:500", "#IDLE,up=10", "#SESSION,START,run",
                            "#H ppsdrops=1", "#H todtrunc=2", "#Ht 0 FX10 n=10 skip=1 stall=0",
                            "#LOG,SensorSync-logger,1,tick_hz=75000000.000,cap=0"}) {
        SensorSyncProtocol bad;
        bad.observe("#SESSION,START,run", false);
        bad.observe(line, false);
        CHECK(bad.failed);
    }
}

namespace {
    Clock::time_point at(double seconds) {
        return Clock::time_point{} + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
    }
} // namespace

TEST_CASE("LogGrowthTracker: GrowthResetsTheStallClock") {
    LogGrowthTracker t;
    t.Reset(0, at(100));
    CHECK(t.Update(0, at(105)) == doctest::Approx(5.0));
    CHECK(t.Update(4096, at(106)) == doctest::Approx(0.0)); // grew now
    CHECK(t.Update(4096, at(109)) == doctest::Approx(3.0)); // ...and stalled again from there
}

TEST_CASE("LogGrowthTracker: StallAccumulatesFromTheLastGrowthNotTheLastPoll") {
    // Polling more often must not make a stall look shorter.
    LogGrowthTracker t;
    t.Reset(1000, at(0));
    for (int i = 1; i <= 20; ++i) {
        CHECK(t.Update(1000, at(i)) == doctest::Approx(static_cast<double>(i)));
    }
}

TEST_CASE("LogGrowthTracker: ASteadyClockIsImmuneToTheWallClockJumpThatUsedToAbortSessions") {
    // The steady clock cannot step; a 1-hour NTP correction of the wall clock
    // happens between two of these polls and changes nothing here.
    LogGrowthTracker t;
    t.Reset(0, at(10));
    CHECK(t.Update(512, at(11)) == doctest::Approx(0.0));
    CHECK(t.Update(1024, at(12)) == doctest::Approx(0.0));
    CHECK(t.Update(1024, at(13)) < 15.0); // well under the abort threshold
}

TEST_CASE("LogGrowthTracker: SizeChangeInEitherDirectionCountsAsActivity") {
    // A truncated or rotated file still means something is writing; the tracker
    // exists to catch a DEAD USB link, not to police the file's shape.
    LogGrowthTracker t;
    t.Reset(8192, at(0));
    CHECK(t.Update(4096, at(5)) == doctest::Approx(0.0));
    CHECK(t.LastSize() == 4096);
}

TEST_CASE("LogGrowthTracker: UnknownStartSizeStillStartsTheClockAtSessionStart") {
    // Start() seeds -1 when the stat fails. Leaving the timestamp at the
    // steady_clock epoch would report an instant multi-year stall.
    LogGrowthTracker t;
    t.Reset(-1, at(1000));
    CHECK(t.Update(-1, at(1002)) == doctest::Approx(2.0));
    CHECK(t.Update(0, at(1003)) == doctest::Approx(0.0)); // the first successful stat counts as growth
}

TEST_CASE("LogGrowthTracker: ResetRebaselinesAWatchedFile") {
    LogGrowthTracker t;
    t.Reset(0, at(0));
    CHECK(t.Update(0, at(30)) == doctest::Approx(30.0));
    t.Reset(0, at(30)); // new session, same (empty) file
    CHECK(t.Update(0, at(31)) == doctest::Approx(1.0));
}
