// CoLa B frames byte-compared against the operating instructions (8023198/1MNR/2024-10-25, annex 12.3).

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "app_config.h"
#include "cola_b.h"
#include "command_builder.h"


namespace {
    using Bytes = std::vector<std::uint8_t>;
    namespace B = lms4xxx::CommandBuilder;
    namespace F = lms4xxx::ScanFixed;

    // "02 02 02 02 00 ..." -> bytes
    Bytes Hex(const std::string &text) {
        Bytes out;
        unsigned value = 0;
        int digits = 0;
        for (const char c: text) {
            int d;
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (c >= 'A' && c <= 'F') {
                d = c - 'A' + 10;
            } else if (c >= 'a' && c <= 'f') {
                d = c - 'a' + 10;
            } else {
                continue;
            }
            value = value * 16 + static_cast<unsigned>(d);
            if (++digits == 2) {
                out.push_back(static_cast<std::uint8_t>(value));
                value = 0;
                digits = 0;
            }
        }
        return out;
    }

    std::string Name(const Bytes &frame) {
        lms4xxx::ColaBMessage msg;
        REQUIRE(!lms4xxx::ColaBCodec::Decode(frame.data() + 8, frame.size() - 9, msg));
        return msg.command_name;
    }

    Bytes Params(const Bytes &frame) { return lms4xxx::ColaBCodec::ParamsOf(frame, Name(frame).size()); }
} // namespace


TEST_CASE("Frames match the manual's CoLa B examples byte for byte") {
    // Table 13 / 20: sMN SetAccessMode 03 F4724744
    CHECK(B::BuildLogin() == Hex("02 02 02 02 00 00 00 17 73 4D 4E 20 53 65 74 41 63 63 65 73 73 4D 6F 64 65 20 03 F4 72 47 44 B3"));
    // Table 64: sMN Run
    CHECK(B::BuildRun() == Hex("02 02 02 02 00 00 00 07 73 4D 4E 20 52 75 6E 19"));
    // Table 76: sWN LMPoutputRange 1 +833 +700000 +1100000
    CHECK(B::BuildOutputRange(833, 700000, 1100000) ==
        Hex("02 02 02 02 00 00 00 21 73 57 4E 20 4C 4D 50 6F 75 74 70 75 74 52 61 6E 67 65 20 00 01 00 00 03 41 00 0A AE 60 00 10 C8 E0 C4"));
    // Table 72: sWN LMDscandatacfg 1 0 1 1 1 1 0 0 0 0 0 +10 (remission only, percent, encoder, every 10th scan)
    CHECK(B::BuildScanDataConfig(lms4xxx::Remission::kRefl, false, 0x01, true, false, 10) ==
        Hex("02 02 02 02 00 00 00 20 73 57 4E 20 4C 4D 44 73 63 61 6E 64 61 74 61 63 66 67 20 01 00 01 01 01 01 00 00 00 00 00 00 0A 49"));
    // Table 127: sWN LFPmedianfilter 1 3
    CHECK(B::BuildMedianFilter(true) ==
        Hex("02 02 02 02 00 00 00 17 73 57 4E 20 4C 46 50 6D 65 64 69 61 6E 66 69 6C 74 65 72 20 01 00 03 38"));
    // Table 135: sWN LFPedgefilter 1
    CHECK(B::BuildEdgeFilter(true) ==
        Hex("02 02 02 02 00 00 00 13 73 57 4E 20 4C 46 50 65 64 67 65 66 69 6C 74 65 72 20 01 32"));
    // Table 147: sWN LFPcubicareafilter 1 +10000 +20000 -15000 +15000 (the manual's checksum does not add up)
    CHECK(Params(B::BuildCubicAreaFilter(0x01, 10000, 20000, -15000, 15000)) ==
        Hex("01 00 00 27 10 00 00 4E 20 FF FF C5 68 00 00 3A 98"));
    // Table 156: sWN LFPglossfilter 1
    CHECK(B::BuildGlossFilter(true) ==
        Hex("02 02 02 02 00 00 00 14 73 57 4E 20 4C 46 50 67 6C 6F 73 73 66 69 6C 74 65 72 20 01 55"));
    // Table 105: sWN TSCRole 1
    CHECK(B::BuildSetTimeSyncRole(1) == Hex("02 02 02 02 00 00 00 0D 73 57 4E 20 54 53 43 52 6F 6C 65 20 01 1B"));
    // Table 110: sWN TSCTCSrvAddr 192.168.0.11
    CHECK(B::BuildSetNtpServer({192, 168, 0, 11}) ==
        Hex("02 02 02 02 00 00 00 15 73 57 4E 20 54 53 43 54 43 53 72 76 41 64 64 72 20 C0 A8 00 0B 3E"));
    // Table 115: sWN TSCTCtimezone +36
    CHECK(B::BuildSetNtpTimezone(36) ==
        Hex("02 02 02 02 00 00 00 13 73 57 4E 20 54 53 43 54 43 74 69 6D 65 7A 6F 6E 65 20 24 16"));
    // Table 119: sWN TSCTCupdatetime +600
    CHECK(B::BuildSetNtpUpdateTime(600) ==
        Hex("02 02 02 02 00 00 00 18 73 57 4E 20 54 53 43 54 43 75 70 64 61 74 65 74 69 6D 65 20 00 00 02 58 67"));
    // Table 48: sMN mSCloadappdef (the configure baseline)
    CHECK(B::BuildLoadAppDefaults() ==
        Hex("02 02 02 02 00 00 00 11 73 4D 4E 20 6D 53 43 6C 6F 61 64 61 70 70 64 65 66 2D"));
    // Table 52: sMN LMCstartmeas
    CHECK(B::BuildStartMeasurement() ==
        Hex("02 02 02 02 00 00 00 10 73 4D 4E 20 4C 4D 43 73 74 61 72 74 6D 65 61 73 68"));
    // Table 90: sEN LMDscandata 1 (the manual's checksum does not add up)
    CHECK(Name(B::BuildStartStream()) == "LMDscandata");
    CHECK(Params(B::BuildStartStream()) == Hex("01"));
    CHECK(Params(B::BuildStopStream()) == Hex("00"));
    // Table 79: sRN LMPoutputRange
    CHECK(B::BuildReadVariable("LMPoutputRange") ==
        Hex("02 02 02 02 00 00 00 12 73 52 4E 20 4C 4D 50 6F 75 74 70 75 74 52 61 6E 67 65 5E"));
    CHECK(Name(B::BuildStandby()) == "LMCstandby");
}


TEST_CASE("The driver's fixed frames have the documented structure") {
    // LMDscandatacfg (table 71): DIST, remission+angle+quality, reserved 1, unit, no encoder,
    // device name off, time stamp only with NTP, every scan
    CHECK(Params(B::BuildScanDataConfig(lms4xxx::Remission::kRssi, true)) == Hex("01 00 07 01 00 00 00 00 00 00 01 00 01"));
    CHECK(Params(B::BuildScanDataConfig(lms4xxx::Remission::kRefl, true)) == Hex("01 00 07 01 01 00 00 00 00 00 01 00 01"));
    CHECK(Params(B::BuildScanDataConfig(lms4xxx::Remission::kRssi, false)) == Hex("01 00 07 01 00 00 00 00 00 00 00 00 01"));

    // LMPoutputRange (table 75 defaults): 1/12 deg, +55 deg .. +125 deg
    CHECK(Params(B::BuildOutputRange()) == Hex("00 01 00 00 03 41 00 08 64 70 00 13 12 D0"));
    CHECK((F::kStopAngle1e4 - F::kStartAngle1e4) / static_cast<int>(F::kAngularResolution1e4) + 1 == F::kPointsPerScan);

    // Cubic filter off with the full box (table 146 ranges)
    const auto cubic = Params(B::BuildCubicAreaFilter(F::kCubicAreaFilterOff, 0, F::kCubicMaxDist1e4mm,
                                                      -F::kCubicExpansion1e4mm, F::kCubicExpansion1e4mm));
    CHECK(cubic[0] == F::kCubicAreaFilterOff);
    CHECK(Bytes(cubic.begin() + 1, cubic.end()) == Hex("00 00 00 00 00 01 66 80 FF FF 50 38 00 00 AF C8"));

    // Remaining off-frames
    CHECK(Params(B::BuildFrontendEdgeFilter(F::kFrontendEdgeFilterOff)) == Bytes{F::kFrontendEdgeFilterOff, 0x01});
    CHECK(Params(B::BuildEdgeFilter(false)) == Hex("00"));
    CHECK(Params(B::BuildMedianFilter(false)) == Hex("00 00 03"));
    CHECK(Params(B::BuildGlossFilter(false)) == Hex("00"));
    CHECK(Params(B::BuildMeanFilter(false, 2)) == Hex("00 00 02 00"));
    CHECK(Params(B::BuildSetNtpTimezone(F::kTimezoneUtc)) == Hex("22"));
}


TEST_CASE("Command names decode to the manual's hex spellings") {
    const auto ascii = [](const std::string &hex) {
        const auto bytes = Hex(hex);
        return std::string(bytes.begin(), bytes.end());
    };
    CHECK(Name(B::BuildLoadAppDefaults()) == ascii("6D 53 43 6C 6F 61 64 61 70 70 64 65 66"));
    CHECK(Name(B::BuildScanDataConfig(lms4xxx::Remission::kRssi, true)) == ascii("4C 4D 44 73 63 61 6E 64 61 74 61 63 66 67"));
    CHECK(Name(B::BuildOutputRange()) == ascii("4C 4D 50 6F 75 74 70 75 74 52 61 6E 67 65"));
    CHECK(Name(B::BuildFrontendEdgeFilter(0)) == ascii("4C 46 50 66 72 6F 6E 74 65 6E 64 45 64 67 65 66 69 6C 74 65 72"));
    CHECK(Name(B::BuildSetNtpTimezone(34)) == ascii("54 53 43 54 43 74 69 6D 65 7A 6F 6E 65"));
}


TEST_CASE("sFA error frames decode to the SOPAS code") {
    // Table 280: sFA 1 (wrong user level), data section "sFA " + Uint16 code
    const auto data = Hex("73 46 41 20 00 01");
    lms4xxx::ColaBMessage msg;
    REQUIRE(!lms4xxx::ColaBCodec::Decode(data.data(), data.size(), msg));
    CHECK(msg.command_type == "sFA");
    CHECK(msg.command_name.empty());
    CHECK(msg.payload == Hex("00 01"));
    CHECK(lms4xxx::ColaBCodec::DecodeUint16(msg.payload.data()) == 1);
    CHECK(std::string(lms4xxx::SopasErrorText(1)).find("user level") != std::string::npos);
    CHECK(std::string(lms4xxx::SopasErrorText(3)).find("variable") != std::string::npos);
}


TEST_CASE("ParamsOf returns exactly the parameter bytes") {
    CHECK(lms4xxx::ColaBCodec::ParamsOf(B::BuildMedianFilter(true), std::string("LFPmedianfilter").size()) == Hex("01 00 03"));
    CHECK(lms4xxx::ColaBCodec::ParamsOf(B::BuildRun(), std::string("Run").size()).empty());
    CHECK(lms4xxx::ColaBCodec::ParamsOf(Bytes{}, 3).empty());
}
