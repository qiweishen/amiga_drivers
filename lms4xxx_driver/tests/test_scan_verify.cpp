// VerifyScanContent: the first telegram must match the fixed measurement setup exactly;
// DeviceTimePlausible: the NTP time lock's notion of a synchronised device clock

#include <doctest/doctest.h>

#include <string>

#include "scan_verify.h"

using namespace lms4xxx;

namespace {
    ChannelData16 Channel16(ChannelContent16 content) {
        ChannelData16 ch;
        ch.content = content;
        ch.num_data = ScanFixed::kPointsPerScan;
        ch.data.resize(ch.num_data);
        ch.angle_step = static_cast<std::uint16_t>(ScanFixed::kAngularResolution1e4);
        ch.start_angle = ScanFixed::kStartAngle1e4;
        return ch;
    }

    ChannelData8 Channel8(ChannelContent8 content) {
        ChannelData8 ch;
        ch.content = content;
        ch.num_data = ScanFixed::kPointsPerScan;
        ch.data.resize(ch.num_data);
        ch.angle_step = static_cast<std::uint16_t>(ScanFixed::kAngularResolution1e4);
        ch.start_angle = ScanFixed::kStartAngle1e4;
        return ch;
    }

    // What the driver's own LMDscandatacfg produces
    ScanData GoodScan(Remission remission, bool ntp) {
        ScanData s;
        s.channels_16bit.push_back(Channel16(ChannelContent16::kDist1));
        s.channels_16bit.push_back(Channel16(remission == Remission::kRefl ? ChannelContent16::kRefl1
                                                                           : ChannelContent16::kRssi1));
        s.channels_16bit.push_back(Channel16(ChannelContent16::kAngl1));
        s.channels_8bit.push_back(Channel8(ChannelContent8::kQlty1));
        s.has_timestamp = ntp;
        return s;
    }

    ScanConfig Config(Remission r) {
        ScanConfig c;
        c.remission = r;
        return c;
    }
} // namespace

TEST_CASE("The configured setup verifies clean for both remission units and both NTP states") {
    for (const Remission r: {Remission::kRssi, Remission::kRefl}) {
        for (const bool ntp: {false, true}) {
            CAPTURE(static_cast<int>(r));
            CAPTURE(ntp);
            CHECK(VerifyScanContent(GoodScan(r, ntp), Config(r), ntp).empty());
        }
    }
}

TEST_CASE("Every deviation is named") {
    SUBCASE("wrong remission unit") {
        const auto problems = VerifyScanContent(GoodScan(Remission::kRssi, false), Config(Remission::kRefl), false);
        CHECK(problems.find("REFL1 missing") != std::string::npos);
    }
    SUBCASE("a missing channel") {
        ScanData s = GoodScan(Remission::kRssi, false);
        s.channels_8bit.clear();
        const auto problems = VerifyScanContent(s, Config(Remission::kRssi), false);
        CHECK(problems.find("QLTY1 missing") != std::string::npos);
        CHECK(problems.find("0 8-bit channels") != std::string::npos);
    }
    SUBCASE("geometry drift") {
        ScanData s = GoodScan(Remission::kRssi, false);
        s.channels_16bit[0].num_data = 840;
        s.channels_16bit[2].start_angle = 0;
        const auto problems = VerifyScanContent(s, Config(Remission::kRssi), false);
        CHECK(problems.find("DIST1: 840 points") != std::string::npos);
        CHECK(problems.find("ANGL1: start 0") != std::string::npos);
    }
    SUBCASE("timestamp block presence follows NTP") {
        CHECK(VerifyScanContent(GoodScan(Remission::kRssi, false), Config(Remission::kRssi), true)
              .find("timestamp block missing") != std::string::npos);
        CHECK(VerifyScanContent(GoodScan(Remission::kRssi, true), Config(Remission::kRssi), false)
              .find("unexpected timestamp block") != std::string::npos);
    }
    SUBCASE("blocks the driver never configures") {
        ScanData s = GoodScan(Remission::kRssi, false);
        s.has_encoder = true;
        s.has_device_name = true;
        const auto problems = VerifyScanContent(s, Config(Remission::kRssi), false);
        CHECK(problems.find("encoder") != std::string::npos);
        CHECK(problems.find("device name") != std::string::npos);
    }
}

TEST_CASE("DeviceTimePlausible separates a synchronised clock from the free-running boot clock") {
    ScanTimestamp ts;
    SUBCASE("the epoch the device boots at is not plausible") {
        ts.year = 1970;
        ts.month = 1;
        ts.day = 1;
        ts.hour = 0;
        ts.minute = 12;
        ts.second = 34;
        CHECK_FALSE(DeviceTimePlausible(ts));
    }
    SUBCASE("a date before the floor is not plausible") {
        ts.year = 2025;
        ts.month = 12;
        ts.day = 31;
        ts.hour = 23;
        ts.minute = 59;
        ts.second = 59;
        ts.microsecond = 999999;
        CHECK_FALSE(DeviceTimePlausible(ts));
    }
    SUBCASE("the floor itself and anything later is plausible") {
        ts.year = 2026;
        ts.month = 1;
        ts.day = 1;
        CHECK(DeviceTimePlausible(ts));
        ts.month = 9;
        ts.day = 7;
        ts.hour = 10;
        ts.minute = 30;
        ts.second = 5;
        ts.microsecond = 123456;
        CHECK(DeviceTimePlausible(ts));
    }
    SUBCASE("malformed fields are never plausible") {
        ts.year = 2026;
        ts.month = 13; // DeviceTimeUnixUs() rejects it as 0
        ts.day = 1;
        CHECK_FALSE(DeviceTimePlausible(ts));
        ts.month = 1;
        ts.microsecond = 1000000;
        CHECK_FALSE(DeviceTimePlausible(ts));
    }
}
