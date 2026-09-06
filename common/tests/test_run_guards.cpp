/// @file test_run_guards.cpp
/// @brief The rig-wide "should this recording continue?" policy.
///
/// common::RunGuards is deliberately free of the filesystem, the clock and the
/// logger, so every branch that can end a recording is reachable from here —
/// the loop that drives it in main.cpp has no test host of its own.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>

#include "run_guards.h"

using common::GuardConfig;
using common::RunGuards;
using Verdict = common::RunGuards::Verdict;

namespace {
    GuardConfig DefaultConfig() {
        GuardConfig cfg;
        cfg.disk_min_free_gib = 5.0;
        cfg.disk_warn_free_gib = 20.0;
        cfg.no_data_warn_s = 5.0;
        cfg.no_data_abort_s = 30.0;
        return cfg;
    }

    constexpr std::uint64_t Seconds(double s) {
        return static_cast<std::uint64_t>(s * 1e6);
    }
} // namespace


TEST_CASE("Disk: the hard floor stops the run, the soft floor only warns") {
    RunGuards guards(DefaultConfig());

    CHECK(guards.CheckDisk(100.0).verdict == Verdict::kOk);
    CHECK(guards.CheckDisk(19.9).verdict == Verdict::kWarn);
    CHECK(guards.CheckDisk(4.9).verdict == Verdict::kStop);

    // Exactly at a threshold is still fine: the floors are what must not be
    // crossed, and "5.0 GiB free with a 5 GiB floor" is not yet a violation.
    RunGuards exact(DefaultConfig());
    CHECK(exact.CheckDisk(20.0).verdict == Verdict::kOk);
    CHECK(exact.CheckDisk(5.0).verdict == Verdict::kWarn); // below warn, at min
}

TEST_CASE("Disk: an unreadable filesystem is not evidence that it is full") {
    // FreeSpaceGiB returns -1 when statvfs fails. Stopping a recording on that
    // would turn a monitoring failure into data loss.
    RunGuards guards(DefaultConfig());
    CHECK(guards.CheckDisk(-1.0).verdict == Verdict::kOk);
    CHECK(guards.CheckDisk(-1.0).message.empty());
}

TEST_CASE("Disk: the warning is latched and re-arms only after recovery") {
    // main calls this once a second. Without the latch, a run parked between
    // the two floors emits one warning per second for hours.
    RunGuards guards(DefaultConfig());
    CHECK(guards.CheckDisk(10.0).verdict == Verdict::kWarn);
    CHECK(guards.CheckDisk(10.0).verdict == Verdict::kOk); // same condition, silent
    CHECK(guards.CheckDisk(9.0).verdict == Verdict::kOk); // still below, still silent

    CHECK(guards.CheckDisk(50.0).verdict == Verdict::kOk); // recovered: re-arm
    CHECK(guards.CheckDisk(10.0).verdict == Verdict::kWarn); // and warn again
}

TEST_CASE("Disk: a zero threshold turns that floor off") {
    GuardConfig cfg = DefaultConfig();
    cfg.disk_min_free_gib = 0.0;
    cfg.disk_warn_free_gib = 0.0;
    RunGuards guards(cfg);
    CHECK(guards.CheckDisk(0.0).verdict == Verdict::kOk);

    GuardConfig hard_only = DefaultConfig();
    hard_only.disk_warn_free_gib = 0.0;
    RunGuards no_warn(hard_only);
    CHECK(no_warn.CheckDisk(10.0).verdict == Verdict::kOk); // warn disabled
    CHECK(no_warn.CheckDisk(1.0).verdict == Verdict::kStop); // hard floor still armed
}

TEST_CASE("Sensor: nullopt means the watchdog does not apply") {
    // An external trigger with no pulses, a warm-up, a driver between reconnect
    // attempts: the driver says so, and no amount of silence trips anything.
    RunGuards guards(DefaultConfig());
    CHECK(guards.CheckSensor("FX10", std::nullopt).verdict == Verdict::kOk);
    CHECK(guards.CheckSensor("FX10", Seconds(3600)).verdict == Verdict::kStop); // it does apply now
    CHECK(guards.CheckSensor("FX10", std::nullopt).verdict == Verdict::kOk); // and off again
}

TEST_CASE("Sensor: warn before abort, and both thresholds are inclusive") {
    RunGuards guards(DefaultConfig());
    CHECK(guards.CheckSensor("GoX", Seconds(1)).verdict == Verdict::kOk);
    CHECK(guards.CheckSensor("GoX", Seconds(5)).verdict == Verdict::kWarn);
    CHECK(guards.CheckSensor("GoX", Seconds(29.9)).verdict == Verdict::kOk); // latched warning
    CHECK(guards.CheckSensor("GoX", Seconds(30)).verdict == Verdict::kStop);
}

TEST_CASE("Sensor: the warning re-arms when data comes back") {
    RunGuards guards(DefaultConfig());
    CHECK(guards.CheckSensor("LMS4xxx", Seconds(6)).verdict == Verdict::kWarn);
    CHECK(guards.CheckSensor("LMS4xxx", Seconds(7)).verdict == Verdict::kOk);
    CHECK(guards.CheckSensor("LMS4xxx", Seconds(0)).verdict == Verdict::kOk); // a frame arrived
    CHECK(guards.CheckSensor("LMS4xxx", Seconds(6)).verdict == Verdict::kWarn); // silent again
}

TEST_CASE("Sensor: an explicit nullopt also clears a pending warning") {
    // A driver that goes from "streaming" to "expected to be quiet" (fx10
    // switching off, gox tearing a session down) must not leave a latched
    // warning behind that suppresses the next real one.
    RunGuards guards(DefaultConfig());
    CHECK(guards.CheckSensor("FX10", Seconds(6)).verdict == Verdict::kWarn);
    CHECK(guards.CheckSensor("FX10", std::nullopt).verdict == Verdict::kOk);
    CHECK(guards.CheckSensor("FX10", Seconds(6)).verdict == Verdict::kWarn);
}

TEST_CASE("Sensor: a zero abort threshold disables the watchdog but keeps the warning") {
    GuardConfig cfg = DefaultConfig();
    cfg.no_data_abort_s = 0.0;
    RunGuards guards(cfg);
    CHECK(guards.CheckSensor("AsteRx", Seconds(3600)).verdict == Verdict::kWarn);
    CHECK(guards.CheckSensor("AsteRx", Seconds(7200)).verdict == Verdict::kOk); // latched, never stops
}

TEST_CASE("Sensor: latches are per sensor, never shared") {
    // main keys them per SLOT, so three LiDARs do not suppress each other.
    RunGuards guards(DefaultConfig());
    CHECK(guards.CheckSensor("LMS4xxx#0", Seconds(6)).verdict == Verdict::kWarn);
    CHECK(guards.CheckSensor("LMS4xxx#1", Seconds(6)).verdict == Verdict::kWarn);
    CHECK(guards.CheckSensor("LMS4xxx#0", Seconds(6)).verdict == Verdict::kOk);
    CHECK(guards.CheckSensor("LMS4xxx#2", Seconds(6)).verdict == Verdict::kWarn);
}

TEST_CASE("Messages carry the numbers, and the caller adds the sensor name") {
    // The name prefix is the GUI's routing key, so it is composed in main
    // (markers.DRIVER_NAME_TO_SENSOR_KEY); the guard only supplies the
    // predicate, and never an empty message on a non-Ok verdict.
    RunGuards guards(DefaultConfig());
    const auto disk = guards.CheckDisk(1.0);
    CHECK(disk.verdict == Verdict::kStop);
    CHECK(disk.message.find("1.0 GiB") != std::string::npos);
    CHECK(disk.message.find("5.0 GiB") != std::string::npos);

    const auto sensor = guards.CheckSensor("FX10", Seconds(31));
    CHECK(sensor.verdict == Verdict::kStop);
    CHECK(sensor.message.find("31.0 s") != std::string::npos);
    CHECK(sensor.message.find("FX10") == std::string::npos);
}

TEST_CASE("An Ok verdict never carries a message") {
    RunGuards guards(DefaultConfig());
    CHECK(guards.CheckDisk(500.0).message.empty());
    CHECK(guards.CheckSensor("GoX", Seconds(0)).message.empty());
    CHECK(guards.CheckSensor("GoX", std::nullopt).message.empty());
}
