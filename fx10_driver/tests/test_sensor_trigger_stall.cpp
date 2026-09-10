#include "../include/log_growth_tracker.h"
#include "../../3rd_party/External/sensor_trigger/session_protocol.h"
#include "../../3rd_party/External/sensor_trigger/session_rates.h"

#include <doctest/doctest.h>

#include <chrono>

// The timing log is the only time source for recorded lines, so "the log has
// stopped growing" aborts a session (fx10_driver_app.cpp). The decision used to
// compare st_mtime against the WALL clock: 1 s granularity, and any NTP step —
// routine on a rig that has just acquired GNSS time — fabricated a stall and
// killed a healthy recording.

using fx10::LogGrowthTracker;
using Clock = LogGrowthTracker::Clock;

TEST_CASE("SensorSync rates: physical channels encode firmware v2 group names") {
    std::string spec, error;
    CHECK(sensorsync::BuildFrequencySpec({{0, 50}}, spec, error));
    CHECK(spec == "FX:50");
    CHECK(sensorsync::BuildFrequencySpec({{1, 25}, {3, 5}}, spec, error));
    CHECK(spec == "FX:25,JAI:5");
    CHECK(sensorsync::BuildFrequencySpec({{0, 50}, {1, 50}, {2, 0}, {3, 0}}, spec, error));
    CHECK(spec == "FX:50,JAI:0");
    trigger::Rates parsed;
    CHECK(trigger::parseRates(spec.c_str(), parsed)); // actual firmware grammar
    CHECK(parsed.hz[0] == 50);
    CHECK(parsed.hz[1] == 0);
    CHECK(sensorsync::BuildFrequencySpec({}, spec, error));
    CHECK(spec.empty()); // retain groups, never emit an explicitly empty freq=
}

TEST_CASE("SensorSync rates: conflicts and invalid group ranges fail before START") {
    std::string spec, error;
    CHECK_FALSE(sensorsync::BuildFrequencySpec({{0, 50}, {1, 0}}, spec, error));
    CHECK(error.find("conflicting") != std::string::npos);
    CHECK_FALSE(sensorsync::BuildFrequencySpec({{2, 5}, {3, 7}}, spec, error));
    CHECK_FALSE(sensorsync::BuildFrequencySpec({{0, 19}}, spec, error));
    CHECK_FALSE(sensorsync::BuildFrequencySpec({{2, 11}}, spec, error));
    CHECK_FALSE(sensorsync::BuildFrequencySpec({{4, 50}}, spec, error));
    CHECK_FALSE(sensorsync::BuildFrequencySpec({{0, std::numeric_limits<double>::infinity()}}, spec, error));
    CHECK(spec.empty());
    const double hz = 50.12345678912345;
    CHECK(sensorsync::BuildFrequencySpec({{0, hz}}, spec, error));
    trigger::Rates decoded;
    CHECK(trigger::parseRates(spec.c_str(), decoded));
    CHECK(decoded.hz[0] == hz); // no %g six-digit narrowing of the requested rate
}

TEST_CASE("SensorSync v2: READY acknowledges hardware start and final counters match cuts") {
    SensorSyncProtocol p;
    p.observe("#SESSION,START,run", false);
    p.observe("#LOG,SensorSync-logger,2,tick_hz=75000000", false);
    CHECK_FALSE(p.ready);
    p.observe("#READY", false);
    CHECK(p.ready);
    p.observe("#H ppsdrops=0 todtrunc=0 tdrops=0 sdrops=0 host=0 pwmerr=0 trunc=0", false);
    p.observe("#TRUNC,0,1,75000000", true);
    p.observe("#HFINAL ppsdrops=0 todtrunc=0 tdrops=0 sdrops=0 host=0 pwmerr=0 trunc=1", true);
    p.observe("#SESSION,STOP,run", true);
    CHECK(p.stop_received);
    CHECK(p.truncations == 1);
    CHECK_FALSE(p.failed);
}

TEST_CASE("SensorSync v2: PWM errors and incomplete stop audits fail") {
    for (const char *bad : {"#READY", "#Ht 0 FX10 n=1 pwmerr=1",
                           "#H ppsdrops=0 todtrunc=0 tdrops=0 sdrops=0 host=0 pwmerr=1",
                           "#HFINAL ppsdrops=0 todtrunc=0 tdrops=0 sdrops=0 host=0 skip=0 stall=0",
                           "#HFINAL ppsdrops=0 todtrunc=0 tdrops=0 sdrops=0 host=0 pwmerr=0 trunc=1",
                           "#TRUNC,2,1,1", "#TRUNC,0,2,1", "#TRUNC,0,1,1,junk"}) {
        SensorSyncProtocol p;
        p.observe("#SESSION,START,run", false);
        p.observe("#LOG,SensorSync-logger,2,tick_hz=75000000", false);
        p.observe("#READY", false);
        p.observe(bad, true);
        CHECK(p.failed);
    }
    SensorSyncProtocol no_header;
    no_header.observe("#SESSION,START,run", false);
    no_header.observe("#READY", false);
    CHECK(no_header.failed);
    CHECK_FALSE(no_header.ready);
}

TEST_CASE("SensorSyncStartupProtocol: FreshBarrierThenIdleSeparatesOldReplies") {
    SensorSyncStartupProtocol p{"#ERR,unknown_command,fresh-token", "#ERR,unknown_command,fresh-token_DONE"};
    for (const char *old : {"#ERR,unknown_command,# SensorSync-logg", "#IDLE,up=1",
                            "#SESSION,STOP,previous-run", "#ERR,unknown_command,old-token"}) {
        p.observe(old);
        CHECK_FALSE(p.barrier_received);
        CHECK_FALSE(p.idle_received);
        CHECK_FALSE(p.failed);
    }
    p.observe("#ERR,unknown_command,fresh-token");
    CHECK(p.barrier_received);
    CHECK_FALSE(p.idle_received);
    p.observe(""); // CRLF separators are harmless
    p.observe("#IDLE,up=1726292");
    CHECK(p.idle_received);
    CHECK_FALSE(p.completed);
    p.observe("#ERR,unknown_command,fresh-token_DONE");
    CHECK(p.completed);
    CHECK_FALSE(p.failed);
}

TEST_CASE("SensorSyncStartupProtocol: BarrierDoesNotMaskNonIdleOrMalformedReplies") {
    for (const char *reply : {"#IDLE,up=", "#IDLE,up=-1", "#IDLE,up=1junk",
                              "#IDLE,up=18446744073709551616", "#H up=1 en=1",
                              "#SESSION,START,unexpected", "#ERR,bad_freq,0:50",
                              "#ERR,unknown_command,fresh-token"}) {
        SensorSyncStartupProtocol p{"#ERR,unknown_command,fresh-token", "#ERR,unknown_command,fresh-token_DONE"};
        p.observe("#ERR,unknown_command,fresh-token");
        p.observe(reply);
        CHECK(p.failed);
        CHECK_FALSE(p.idle_received);
    }
}

TEST_CASE("SensorSyncStartupProtocol: CompletionRequiresIdleAndCannotRepeat") {
    SensorSyncStartupProtocol missing_idle{"#ERR,unknown_command,begin", "#ERR,unknown_command,end"};
    missing_idle.observe("#ERR,unknown_command,begin");
    missing_idle.observe("#ERR,unknown_command,end");
    CHECK(missing_idle.failed);
    CHECK_FALSE(missing_idle.completed);

    SensorSyncStartupProtocol repeated{"#ERR,unknown_command,begin", "#ERR,unknown_command,end"};
    repeated.observe("#ERR,unknown_command,begin");
    repeated.observe("#IDLE,up=2");
    repeated.observe("#ERR,unknown_command,end");
    repeated.observe("#ERR,unknown_command,end");
    CHECK(repeated.failed);
}

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
