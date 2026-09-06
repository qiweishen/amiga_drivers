// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>

#include "warmup_gate.h"

using asterx::WarmupGate;

TEST_CASE("WarmupGate: WaitsForUptimeAndFinetime") {
    WarmupGate g;
    g.min_uptime_s = 1200;
    g.require_finetime = true;
    CHECK_FALSE(g.Ready());
    CHECK(g.Progress() == "no ReceiverStatus received yet");

    CHECK_FALSE(g.Observe(432, 0));
    CHECK_FALSE(g.Ready());
    CHECK(g.Progress() == "up 07:12 / 20:00, FINETIME not yet set");

    CHECK_FALSE(g.Observe(1199, WarmupGate::kFineTime));
    CHECK_FALSE(g.Ready());
    CHECK(g.Progress() == "up 19:59 / 20:00");

    CHECK_FALSE(g.Observe(1200, 1u << 5)); // TOWSET only, not FINETIME
    CHECK_FALSE(g.Ready());

    CHECK_FALSE(g.Observe(1201, WarmupGate::kFineTime | 0x100));
    CHECK(g.Ready());
}

TEST_CASE("WarmupGate: FinetimeOptionalAndUptimeGateOff") {
    WarmupGate g;
    g.min_uptime_s = 1200;
    g.require_finetime = false;
    g.Observe(1200, 0);
    CHECK(g.Ready());

    WarmupGate off;
    off.min_uptime_s = 0;
    off.require_finetime = false;
    CHECK(off.Ready()); // no status needed at all
    off.Observe(3, 0);
    CHECK(off.Ready());

    WarmupGate finetime_only;
    finetime_only.min_uptime_s = 0;
    finetime_only.require_finetime = true;
    CHECK_FALSE(finetime_only.Ready());
    finetime_only.Observe(5, WarmupGate::kFineTime);
    CHECK(finetime_only.Ready());
    CHECK(finetime_only.Progress() == "up 00:05");
}

TEST_CASE("WarmupGate: DetectsReceiverReset") {
    WarmupGate g;
    CHECK_FALSE(g.Observe(100, 0));
    CHECK_FALSE(g.Observe(101, 0));
    CHECK(g.Observe(3, 0)); // up-time went backwards
    CHECK_FALSE(g.Observe(4, 0));
    g.Reset();
    CHECK_FALSE(g.up_time_s.has_value());
    CHECK_FALSE(g.Observe(1, 0));
}

TEST_CASE("WarmupGate: FormatsHms") {
    CHECK(asterx::FormatHms(0) == "00:00");
    CHECK(asterx::FormatHms(1201) == "20:01");
    CHECK(asterx::FormatHms(3725) == "1:02:05");
}
