// GevIEEE1588Status interpretation for the camera's PTP slave role
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
