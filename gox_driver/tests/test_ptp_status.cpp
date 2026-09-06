// GevIEEE1588Status / GevIEEE1588ClockAccuracy interpretation
// (src/ptp_status.cpp, manual p.128).

#include "ptp_status.h"

#include <doctest/doctest.h>

TEST_CASE("ptp_status: the healthy state is recognised by name and by value") {
    CHECK(gox::ClassifyPtpStatus("Slave") == gox::PtpState::kSlave);
    CHECK(gox::ClassifyPtpStatus("slave") == gox::PtpState::kSlave);
    CHECK(gox::ClassifyPtpStatus("SLAVE") == gox::PtpState::kSlave);
    CHECK(gox::ClassifyPtpStatus(" Slave ") == gox::PtpState::kSlave);
    CHECK(gox::ClassifyPtpStatus("9") == gox::PtpState::kSlave); // 9: slave
}

TEST_CASE("ptp_status: master and faulty are the two hard failures") {
    // Master = no grandmaster is winning the BMCA on this network.
    CHECK(gox::ClassifyPtpStatus("Master") == gox::PtpState::kMaster);
    CHECK(gox::ClassifyPtpStatus("6") == gox::PtpState::kMaster);
    // Faulty = the camera's own 1588 stack hit an internal error.
    CHECK(gox::ClassifyPtpStatus("Faulty") == gox::PtpState::kFaulty);
    CHECK(gox::ClassifyPtpStatus("2") == gox::PtpState::kFaulty);
}

TEST_CASE("ptp_status: every converging state is Other") {
    for (const char *s: {"Initializing", "Disabled", "Listening", "PreMaster", "Passive", "Uncalibrated",
                         "1", "3", "4", "5", "7", "8", "", "nonsense"}) {
        CAPTURE(s);
        CHECK(gox::ClassifyPtpStatus(s) == gox::PtpState::kOther);
    }
}

TEST_CASE("ptp_status: usable clock accuracy is 0..9, i.e. 1 ms or better") {
    for (int64_t a = 0; a <= 9; ++a) {
        CAPTURE(a);
        CHECK(gox::PtpClockAccuracyOk(a)); // Within25ns .. Within1ms
    }
    for (int64_t a = 10; a <= 20; ++a) {
        CAPTURE(a);
        CHECK_FALSE(gox::PtpClockAccuracyOk(a)); // 2.5 ms or worse, 19 Unknown, 20 Reserved
    }
    CHECK_FALSE(gox::PtpClockAccuracyOk(-1));
    CHECK_FALSE(gox::PtpClockAccuracyOk(19)); // the factory value: not evidence of sync
}

TEST_CASE("ptp_status: a clock accuracy exposed as text maps back to the manual's value") {
    int64_t value = -1;
    // p.128's own spellings, and the guard's decision boundary either side of it.
    REQUIRE(gox::PtpClockAccuracyFromName("Within25ns", value));
    CHECK(value == 0);
    REQUIRE(gox::PtpClockAccuracyFromName(" within1ms ", value)); // trimmed, case-insensitive
    CHECK(value == 9);
    CHECK(gox::PtpClockAccuracyOk(value));
    REQUIRE(gox::PtpClockAccuracyFromName("Within2p5ms", value));
    CHECK(value == 10);
    CHECK_FALSE(gox::PtpClockAccuracyOk(value));
    REQUIRE(gox::PtpClockAccuracyFromName("Unknown", value)); // the factory reading
    CHECK(value == 19);
    CHECK_FALSE(gox::PtpClockAccuracyOk(value));
    REQUIRE(gox::PtpClockAccuracyFromName("Reserved", value));
    CHECK(value == 20);

    // Firmware that prints the number instead of the name.
    REQUIRE(gox::PtpClockAccuracyFromName("6", value));
    CHECK(value == 6);

    // Anything else must fail rather than silently read as 0 (the best accuracy).
    for (const char *bad: {"", "   ", "Within1Millisecond", "-1", "6ns", "nonsense"}) {
        CAPTURE(bad);
        int64_t out = -12345;
        CHECK_FALSE(gox::PtpClockAccuracyFromName(bad, out));
        CHECK(out == -12345); // untouched on failure
    }
}
