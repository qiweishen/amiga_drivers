// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>

#include "warmup_gate.h"

using asterx::WarmupGate;

TEST_CASE("WarmupGate: WaitsForRecordingDurationAndFinetime") {
    using namespace std::chrono;
    const WarmupGate::Clock::time_point start{};
    WarmupGate g;
    g.min_recording_s = 1200;
    g.require_finetime = true;
    CHECK_FALSE(g.Ready(start));
    CHECK(g.Progress(start) == "waiting for first recorded SBF block");

    // An already warm receiver cannot satisfy this session's recording duration.
    CHECK_FALSE(g.Observe(86400, WarmupGate::kFineTime));
    CHECK_FALSE(g.Ready(start + hours(1)));
    CHECK_FALSE(g.RecordingElapsedSeconds(start + hours(1)).has_value());
    g.StartRecording(start);
    CHECK_FALSE(g.Ready(start));

    CHECK_FALSE(g.Observe(86401, 0));
    CHECK_FALSE(g.Ready(start + seconds(432)));
    CHECK(g.Progress(start + seconds(432)) == "recorded 07:12 / 20:00, FINETIME not yet set");

    CHECK_FALSE(g.Observe(90000, WarmupGate::kFineTime));
    CHECK_FALSE(g.Ready(start + seconds(1199) + milliseconds(999)));
    CHECK(g.Progress(start + seconds(1199)) == "recorded 19:59 / 20:00");

    CHECK_FALSE(g.Observe(90001, 1u << 5)); // TOWSET only, not FINETIME
    CHECK_FALSE(g.Ready(start + seconds(1200)));

    CHECK_FALSE(g.Observe(90002, WarmupGate::kFineTime | 0x100));
    CHECK(g.Ready(start + seconds(1200))); // exact elapsed boundary
}

TEST_CASE("WarmupGate: RecordingDurationNeedsNoReceiverStatusWhenFinetimeOptional") {
    using namespace std::chrono;
    const WarmupGate::Clock::time_point start{};
    WarmupGate g;
    g.min_recording_s = 1200;
    g.require_finetime = false;
    g.StartRecording(start);
    CHECK_FALSE(g.Ready(start + seconds(1199)));
    CHECK(g.Ready(start + seconds(1200)));
    g.Observe(1, 0); // a small receiver uptime cannot hold the gate closed
    CHECK(g.Ready(start + seconds(1200)));
}

TEST_CASE("WarmupGate: DurationGateOffAndFinetimeOnly") {
    const WarmupGate::Clock::time_point start{};
    WarmupGate off;
    off.min_recording_s = 0;
    off.require_finetime = false;
    CHECK(off.Ready(start)); // gate explicitly disabled, no block/status needed
    off.Observe(3, 0);
    CHECK(off.Ready(start));

    WarmupGate finetime_only;
    finetime_only.min_recording_s = 0;
    finetime_only.require_finetime = true;
    finetime_only.StartRecording(start);
    CHECK_FALSE(finetime_only.Ready(start));
    finetime_only.Observe(5, WarmupGate::kFineTime);
    CHECK(finetime_only.Ready(start));
    CHECK(finetime_only.Progress(start) == "recorded 00:00");
}

TEST_CASE("WarmupGate: LaterBlocksAndReceiverResetDoNotRestartRecordingDuration") {
    using namespace std::chrono;
    const WarmupGate::Clock::time_point start{};
    WarmupGate g;
    g.min_recording_s = 90;
    g.StartRecording(start);
    g.StartRecording(start + seconds(80)); // another queued block, same recording
    CHECK_FALSE(g.Observe(100, 0));
    CHECK_FALSE(g.Observe(101, 0));
    CHECK(g.Observe(3, 0)); // up-time went backwards
    CHECK_FALSE(g.Observe(4, 0));
    CHECK(g.Ready(start + seconds(90)));
    REQUIRE(g.RecordingElapsedSeconds(start + seconds(90)).has_value());
    CHECK(*g.RecordingElapsedSeconds(start + seconds(90)) == 90);

    g.require_finetime = true;
    g.Observe(5, WarmupGate::kFineTime);
    CHECK(g.Ready(start + seconds(90)));
    g.ResetReceiverStatus(); // reconnect needs fresh FINETIME, same SBF timer
    CHECK_FALSE(g.up_time_s.has_value());
    CHECK_FALSE(g.Ready(start + seconds(90)));
    CHECK_FALSE(g.Observe(1, WarmupGate::kFineTime));
    CHECK(g.Ready(start + seconds(90)));

    g.Reset();
    CHECK_FALSE(g.up_time_s.has_value());
    CHECK_FALSE(g.RecordingElapsedSeconds(start + seconds(90)).has_value());
    CHECK_FALSE(g.Observe(1, 0));
    CHECK_FALSE(g.Ready(start + seconds(90)));
}

TEST_CASE("WarmupGate: FormatsHms") {
    CHECK(asterx::FormatHms(0) == "00:00");
    CHECK(asterx::FormatHms(1201) == "20:01");
    CHECK(asterx::FormatHms(3725) == "1:02:05");
}
