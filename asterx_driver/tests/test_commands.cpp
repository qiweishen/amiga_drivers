// SPDX-License-Identifier: BSD-3-Clause
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "commands.h"

namespace {
    // Minimal settings a receiver would accept: the yaml is the source of truth,
    // so the C++ struct carries no default stream table any more.
    asterx::ReceiverSettings MakeSettings() {
        asterx::ReceiverSettings s;
        s.user = "admin";
        s.password = "secret";
        s.antenna.lever_arm_m = asterx::Vec3{0.1, -0.2, 0.3};
        s.sbf_streams = {
            {1, {"Measurements"}, "OnChange"},
            {8, {"Status"}, "OnChange"},
        };
        return s;
    }

    std::size_t CountKind(const std::vector<asterx::Command> &cmds, asterx::CommandKind kind) {
        return static_cast<std::size_t>(
            std::count_if(cmds.begin(), cmds.end(),
                          [kind](const asterx::Command &c) { return c.kind == kind; }));
    }

    std::size_t CountPrefix(const std::vector<asterx::Command> &cmds, const std::string &prefix) {
        return static_cast<std::size_t>(
            std::count_if(cmds.begin(), cmds.end(),
                          [&prefix](const asterx::Command &c) { return c.text.rfind(prefix, 0) == 0; }));
    }

    std::size_t IndexOf(const std::vector<asterx::Command> &cmds, const std::string &text_prefix) {
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            if (cmds[i].text.rfind(text_prefix, 0) == 0) {
                return i;
            }
        }
        return cmds.size();
    }
} // namespace

TEST_CASE("Commands: ValidationRequiresLeverArmAndStreams") {
    asterx::ReceiverSettings bare;
    // No lever arm and no streams: both are required
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(bare), asterx::ConfigError);

    auto s = MakeSettings();
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(s));

    s.sbf_streams = {{1, {"Measurements"}, "OnChange"}};
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError); // no ReceiverStatus

    s.sbf_streams = {{1, {"ReceiverStatus"}, "sec1"}};
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(s));

}

TEST_CASE("Commands: BuildsLongSbfOutputWithoutTruncation") {
    asterx::SbfStream stream;
    stream.stream_id = 6;
    stream.interval = "OnChange";
    for (int i = 0; i < 50; ++i) {
        stream.blocks.push_back("Block" + std::to_string(i));
    }

    const auto cmd = asterx::BuildSbfOutputCommand(stream, "IP10");
    CHECK(cmd.size() > 256u);
    CHECK(cmd.size() < 2000u);
    CHECK(cmd.find("setSBFOutput, Stream6, IP10, ") != std::string::npos);
    CHECK(cmd.find("Block0+Block1") != std::string::npos);
    CHECK(cmd.find("Block49") != std::string::npos);
}

TEST_CASE("Commands: BuildsLeverArmWithReceiverPrecision") {
    CHECK(asterx::BuildInsAntLeverArmCommand(asterx::Vec3{0.0, 0.0, 0.0}) == "setINSAntLeverArm, 0.000, 0.000, 0.000");
    CHECK(asterx::BuildInsAntLeverArmCommand(asterx::Vec3{-0.0001, 1.2344, -2.3456}) == "setINSAntLeverArm, 0.000, 1.234, -2.346");
}

TEST_CASE("Commands: RejectsOversizedSbfOutputCommand") {
    asterx::SbfStream stream;
    stream.stream_id = 1;
    stream.interval = "OnChange";
    for (int i = 0; i < 250; ++i) {
        stream.blocks.push_back("VeryLongSyntheticBlockName" + std::to_string(i));
    }

    CHECK_THROWS_AS((void) asterx::BuildSbfOutputCommand(stream, "IP10"), asterx::ConfigError);
}

TEST_CASE("Commands: RejectsInvalidDescriptor") {
    asterx::SbfStream stream;
    stream.stream_id = 1;
    stream.interval = "OnChange";
    stream.blocks = {"ExtSensorMeas"};

    CHECK_THROWS_AS((void) asterx::BuildSbfOutputCommand(stream, ""), asterx::ConfigError);
    CHECK_THROWS_AS((void) asterx::BuildSbfOutputCommand(stream, "IP10, none"), asterx::ConfigError);
    CHECK_THROWS_AS((void) asterx::BuildCommandList(MakeSettings(), ""), asterx::ConfigError);
}

TEST_CASE("Commands: RejectsStreamIntervalOff") {
    // "off" would satisfy every other check while never delivering a block —
    // for the ReceiverStatus stream that means the warm-up gate never opens.
    CHECK_FALSE(asterx::IsValidSbfInterval("off"));
    CHECK(asterx::IsValidSbfInterval("OnChange"));
    CHECK(asterx::IsValidSbfInterval("sec1"));

    auto s = MakeSettings();
    s.sbf_streams = {{8, {"Status"}, "off"}};
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError);
}

TEST_CASE("Commands: CommandListFollowsConfigureSequence") {
    const auto settings = MakeSettings();
    const auto cmds = asterx::BuildCommandList(settings, "IP12");

    REQUIRE_FALSE(cmds.empty());
    // One login form only: the long version is always accepted and its extra
    // arguments are ignored when a full-control account already exists (p.18).
    const std::string login = "login, admin, secret, RxAdmin, S3pt3ntr10";
    CHECK(cmds[0].text == login);
    CHECK(cmds[0].kind == asterx::CommandKind::kPlain);
    CHECK(cmds[1].text == "exeCopyConfigFile, RxDefault, Current");
    CHECK(cmds[2].text == login);
    CHECK(cmds[3].text == "getReceiverCapabilities");
    CHECK(cmds[3].kind == asterx::CommandKind::kCheckCapabilities);
    CHECK(CountPrefix(cmds, "login") == 2u);

    // Exactly one of each bookkeeping step; both antenna types are Unknown by
    // default, so no antenna list lookup.
    CHECK(CountKind(cmds, asterx::CommandKind::kCheckCapabilities) == 1u);
    CHECK(CountKind(cmds, asterx::CommandKind::kListConfig) == 1u);
    CHECK(CountKind(cmds, asterx::CommandKind::kListAntennaInfo) == 0u);

    // Every configuration write is immediately double-checked by a readback,
    // and every readback is a VerifyEcho.
    std::size_t sets = 0;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].text.rfind("set", 0) != 0) {
            continue;
        }
        ++sets;
        REQUIRE_MESSAGE((i + 1 < cmds.size()), cmds[i].text);
        CHECK_MESSAGE((cmds[i + 1].text.rfind("get", 0) == 0u), cmds[i].text << " is not followed by a readback but by " << cmds[i + 1].text);
        CHECK_MESSAGE((cmds[i + 1].kind == asterx::CommandKind::kVerifyEcho), cmds[i].text);
    }
    CHECK(CountKind(cmds, asterx::CommandKind::kVerifyEcho) == sets);
    CHECK(cmds.size() == 3u + 1u + 2u * sets + 1u);

    // The reset runs before every read-only step: a previous session's SBF
    // streams are still armed on this IPxx until it does.
    const auto reset = IndexOf(cmds, "exeCopyConfigFile, RxDefault, Current");
    CHECK(IndexOf(cmds, "login") < reset);
    CHECK(reset < IndexOf(cmds, "getReceiverCapabilities"));
    CHECK(reset < IndexOf(cmds, "lstConfigFile, Current"));
    for (const auto &c: cmds) {
        CHECK_MESSAGE((c.text.rfind("setUserAccessLevel", 0) == std::string::npos), c.text);
        CHECK_MESSAGE((c.text.rfind("lstCurrentUser", 0) == std::string::npos), c.text);
    }

    // The receiver is the rig's time source: PPS/NTP/PTP and the NMEA pins come
    // first after the reset so a reconnect restores them quickly.
    const auto pps = IndexOf(cmds, "setPPSParameters");
    REQUIRE(pps < cmds.size());
    CHECK(pps - reset < 10u);
    CHECK(pps < IndexOf(cmds, "setNTPServer"));
    CHECK(IndexOf(cmds, "setNTPServer") < IndexOf(cmds, "setPTPServer"));
    CHECK(IndexOf(cmds, "setPTPServer") < IndexOf(cmds, "setDataInOut, IP12, , +SBF"));

    // The "after" listing is read before SBF streams on this socket.
    const auto after = IndexOf(cmds, "lstConfigFile, Current");
    REQUIRE(after < cmds.size());
    CHECK(cmds[after].kind == asterx::CommandKind::kListConfig);
    CHECK(IndexOf(cmds, "getINSAntLeverArm") < after);
    CHECK(after < IndexOf(cmds, "setSBFOutput, Stream1, IP12"));

    // SBF streams are targeted at our own connection descriptor.
    CHECK(CountPrefix(cmds, "setSBFOutput, Stream") == settings.sbf_streams.size());
    for (const auto &c: cmds) {
        if (c.text.rfind("setSBFOutput, Stream", 0) == 0) {
            CHECK_MESSAGE((c.text.find(", IP12, ") != std::string::npos), c.text);
        }
    }

    // Command texts, with the defaults of MakeSettings().
    for (const char *set: {"setPPSParameters, sec1, Low2High, 0.00, GPS, 60, 5.000000",
                           "setNTPServer, off", "setPTPServer, off", "setDataInOut, IP12, , +SBF",
                           "setAntennaType, Main, Unknown", "setAntennaType, Aux1, Unknown",
                           "setAttitudeOffset, 0.000, 0.000", "setINSAntLeverArm, 0.100, -0.200, 0.300",
                           "setCN0Mask, all, 0", "setElevationMask, all, 0", "setSatelliteTracking, all",
                           "setSatelliteUsage, all", "setSatelliteHealthOverride, none, none",
                           "setSignalTracking, all", "setSignalUsage, all, all",
                           "setIMUStartupDataMode, GnssTimeKnown", "setIMUOrientation, SensorDefault",
                           "setGNSSAttitude, MultiAntenna", "setPVTMode, Rover, all",
                           "setReceiverDynamics, Moderate", "setVehicleApplication, Unknown",
                           "setClockSyncThreshold, usec500", "setMultipathMitigation, on, on",
                           "setINSNavConfig, on, all, POI1", "setNtripSettings, NTR1, off"}) {
        CHECK_MESSAGE((CountPrefix(cmds, set) == 1u), set);
    }

    // Readback expectations that pin the reply layout.
    CHECK(cmds[IndexOf(cmds, "getPVTMode")].verify_key == "PVTMode");
    CHECK(cmds[IndexOf(cmds, "getPVTMode")].verify_fields == (std::vector<std::string>{"Rover"}));
    CHECK(cmds[IndexOf(cmds, "getNtripSettings, NTR1")].verify_fields == (std::vector<std::string>{"NTR1", "off"}));
    CHECK(cmds[IndexOf(cmds, "getSBFOutput, Stream1")].verify_fields == (std::vector<std::string>{"Stream1", "IP12", "", "OnChange"}));
    CHECK(cmds[IndexOf(cmds, "getDataInOut, IP12")].verify_fields == (std::vector<std::string>{"IP12"}));
    CHECK(cmds[IndexOf(cmds, "getPPSParameters")].verify_fields == (std::vector<std::string>{"sec1", "Low2High", "0.00", "GPS", "60", "5.000000"}));
    CHECK(cmds[IndexOf(cmds, "getINSAntLeverArm")].verify_fields == (std::vector<std::string>{"0.100", "-0.200", "0.300"}));
    CHECK(cmds[IndexOf(cmds, "getIMUOrientation")].verify_fields == (std::vector<std::string>{"SensorDefault"}));
    CHECK(cmds[IndexOf(cmds, "getGNSSAttitude")].verify_fields == (std::vector<std::string>{"MultiAntenna"}));
    // Signal list echoes are expanded lists with no deterministic field
    CHECK(cmds[IndexOf(cmds, "getSignalTracking")].verify_fields.empty());
    CHECK(cmds[IndexOf(cmds, "getSatelliteHealthOverride")].verify_fields == (std::vector<std::string>{"none", "none"}));
}

TEST_CASE("Commands: ImuOrientationReadbackCarriesThetas") {
    auto s = MakeSettings();
    s.imu.orientation_mode = "manual";
    s.imu.theta_x_deg = -90.0;
    s.imu.theta_z_deg = 1.5;

    const auto cmds = asterx::BuildCommandList(s, "IP12");
    CHECK(asterx::BuildImuOrientationCommand(s.imu) == "setIMUOrientation, manual, -90.000, 0.000, 1.500");
    CHECK(cmds[IndexOf(cmds, "getIMUOrientation")].verify_fields == (std::vector<std::string>{"manual", "-90.000", "0.000", "1.500"}));
}

TEST_CASE("Commands: CommandListLooksUpAntennaNamesOnlyWhenSet") {
    auto settings = MakeSettings();
    settings.antenna.main_type = "AERAT2775_159   SPKE";

    const auto cmds = asterx::BuildCommandList(settings, "IP12");
    const auto lai = IndexOf(cmds, "lstAntennaInfo, Overview");
    REQUIRE(lai < cmds.size());
    CHECK(cmds[lai].kind == asterx::CommandKind::kListAntennaInfo);
    // After the Reset (so the previous session's streams are already off) and
    // before the type is used.
    CHECK(IndexOf(cmds, "exeCopyConfigFile") < lai);
    CHECK(lai < IndexOf(cmds, "setAntennaType, Main"));
    CHECK(IndexOf(cmds, "setAntennaType, Main, \"AERAT2775_159   SPKE\"") < cmds.size());
    CHECK(cmds[IndexOf(cmds, "getAntennaType, Main")].verify_fields == (std::vector<std::string>{"Main", "AERAT2775_159   SPKE"}));
}

TEST_CASE("Commands: BuildsNmeaPinOutputCommand") {
    asterx::NmeaPinStream stream;
    stream.stream_id = 8;
    stream.descriptor = "COM2";
    stream.messages = {"ZDA"};
    stream.interval = "OnChange";
    CHECK(asterx::BuildNmeaOutputCommand(stream) == "setNMEAOutput, Stream8, COM2, ZDA, OnChange");

    stream.messages = {"GGA", "ZDA"};
    stream.interval = "sec1";
    CHECK(asterx::BuildNmeaOutputCommand(stream) == "setNMEAOutput, Stream8, COM2, GGA+ZDA, sec1");
}

TEST_CASE("Commands: PinDescriptorsMatchTheReceiverTable") {
    // Cd column of setNMEAOutput / setDataInOut / setSBFOutput (p.193, p.168, p.202)
    for (const char *ok: {"COM1", "com2", "USB1", "USB2", "IP10", "IP17", "NTR3", "IPS5", "IPR1", "LOG1", "LOG2"}) {
        CHECK_MESSAGE(asterx::IsValidPinDescriptor(ok), ok);
    }
    // This receiver has two COM ports, and DSKx is a disk id, not a connection
    for (const char *bad: {"COM3", "COM4", "COM9", "DSK1", "IP18", "IPS6", "NTR4", "", "LOG3"}) {
        CHECK_FALSE_MESSAGE(asterx::IsValidPinDescriptor(bad), bad);
    }
}

TEST_CASE("Commands: RejectsInvalidNmeaPinStream") {
    const asterx::NmeaPinStream good{8, "COM2", {"ZDA"}, "OnChange"};
    CHECK_NOTHROW((void) asterx::BuildNmeaOutputCommand(good));

    auto bad = good;
    bad.stream_id = 11;
    CHECK_THROWS_AS((void) asterx::BuildNmeaOutputCommand(bad), asterx::ConfigError);

    bad = good;
    bad.descriptor = "COM3"; // not a port of this receiver
    CHECK_THROWS_AS((void) asterx::BuildNmeaOutputCommand(bad), asterx::ConfigError);
    bad.descriptor = "";
    CHECK_THROWS_AS((void) asterx::BuildNmeaOutputCommand(bad), asterx::ConfigError);

    bad = good;
    bad.messages = {"GSV"}; // common NMEA sentence, but not in the receiver's set
    CHECK_THROWS_AS((void) asterx::BuildNmeaOutputCommand(bad), asterx::ConfigError);
    bad.messages.clear();
    CHECK_THROWS_AS((void) asterx::BuildNmeaOutputCommand(bad), asterx::ConfigError);

    bad = good;
    bad.interval = "sec3";
    CHECK_THROWS_AS((void) asterx::BuildNmeaOutputCommand(bad), asterx::ConfigError);
}

TEST_CASE("Commands: ValidationRejectsDuplicatePinStreamIds") {
    auto settings = MakeSettings();
    settings.nmea_streams = {
        {8, "COM2", {"ZDA"}, "OnChange"},
        {8, "COM1", {"GGA"}, "sec1"},
    };
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);

    settings.nmea_streams[1].stream_id = 9;
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));
}

TEST_CASE("Commands: CommandListConfiguresPinStreams") {
    auto settings = MakeSettings();
    settings.nmea_streams = {
        {1, "COM2", {"ZDA"}, "OnChange"},
        {2, "COM2", {"GGA"}, "sec1"},
    };

    const auto cmds = asterx::BuildCommandList(settings, "IP12");

    // The pin port is switched to NMEA output exactly once, before its streams.
    const auto port_enable = IndexOf(cmds, "setDataInOut, COM2, , +NMEA");
    REQUIRE(port_enable < cmds.size());
    CHECK(CountPrefix(cmds, "setDataInOut, COM2, , +NMEA") == 1u);

    const auto zda = IndexOf(cmds, "setNMEAOutput, Stream1, COM2, ZDA, OnChange");
    const auto gga = IndexOf(cmds, "setNMEAOutput, Stream2, COM2, GGA, sec1");
    REQUIRE(zda < cmds.size());
    REQUIRE(gga < cmds.size());
    CHECK(port_enable < zda);
    // Pin output is part of the time-critical group: before the SBF plumbing
    CHECK(gga < IndexOf(cmds, "setDataInOut, IP12, , +SBF"));
    CHECK(gga < IndexOf(cmds, "lstConfigFile, Current"));
}

TEST_CASE("Commands: RedactsOnlyTheNtripPassword") {
    // The sensor-local account is logged in clear
    CHECK(asterx::RedactCmd("login, admin, secret") == "login, admin, secret");
    CHECK(asterx::RedactCmd("setUserAccessLevel, User3, admin, secret, User") == "setUserAccessLevel, User3, admin, secret, User");
    CHECK(asterx::RedactCmd("setDataInOut, IP10, , +SBF") == "setDataInOut, IP10, , +SBF");
    // NTRIP: only the password goes; caster/user/mount stay visible
    CHECK(asterx::RedactCmd("setNtripSettings, NTR1, Client, ntrip.com, 2101, USER, PWD, MP1, v2, auto") == "setNtripSettings, NTR1, Client, ntrip.com, 2101, USER, <REDACTED>, MP1, v2, auto");
    CHECK(asterx::RedactCmd("$R: snts, NTR1, Client, ntrip.com, 2101, USER, PWD, MP1, v2, auto\r\n"
                                 "  NtripSettings, NTR1, Client, ntrip.com, 2101, USER, DD3BQT8GBF, MP1, v2, auto") == "$R: snts, NTR1, Client, ntrip.com, 2101, USER, <REDACTED>, MP1, v2, auto\n"
              "NtripSettings, NTR1, Client, ntrip.com, 2101, USER, <REDACTED>, MP1, v2, auto");
    CHECK(asterx::RedactCmd("setNtripSettings, NTR1, off") == "setNtripSettings, NTR1, off");
    CHECK(asterx::RedactCmd("#setNtripSettings, NTR1, Client, c, 1, u, p, m, v2, auto") == "#setNtripSettings, NTR1, Client, c, 1, u, <REDACTED>, m, v2, auto");
}

TEST_CASE("Commands: ValidationRejectsBadCredentials") {
    auto settings = MakeSettings();
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));

    settings.user = "";
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.user = "RxAdmin";
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.user = "amiga,1"; // ',' separates command arguments
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.user = "seventeen_chars_x"; // login UserName is limited to 16 (p.87)
    CHECK(settings.user.size() == 17u);
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.user = "sixteen_chars_ok";
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));

    settings.password = "";
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.password = "a\"b";
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.password = std::string(33, 'p'); // login Password is limited to 32 (p.87)
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.password = std::string(32, 'p');
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));
    settings.password = "pw";
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));
}

TEST_CASE("Commands: ParsesConfigListingAsSsnRxDeliversIt") {
    // SsnRx strips the prompt and the "---->" pseudo-prompt from every reply and
    // formatted block; the Session concatenates what is left.
    const std::string listing =
            "$R; lstConfigFile, Current\r\n"
            "$-- BLOCK 1 / 1\r\n"
            "  #setIPSettings, Static, 10.95.2.102, 255.255.255.0, 10.95.2.1, , 0.0.0.0, 0.0.0.0, 0\r\n"
            "  setUserAccessLevel, User1, \"amiga\", \"Xy12ab\", User\r\n"
            "  setSBFOutput, Stream1, IP12, ExtSensorMeas, OnChange\r\n"
            "  setElevationMask, all, 15\r\n";
    const auto lines = asterx::ParseConfigFileListing(listing);
    REQUIRE(lines.size() == 4u);
    CHECK(lines[0].permanent);
    CHECK(lines[0].command_name == "setIPSettings");
    CHECK_FALSE(lines[1].permanent);
    CHECK(lines[1].command_name == "setUserAccessLevel");
    CHECK(lines[2].command_name == "setSBFOutput");
    CHECK(lines[3].command_name == "setElevationMask");
    CHECK(lines[3].full_line == "setElevationMask, all, 15");

    // Multi-block shape, frames concatenated by the Session
    const std::string blocks =
            "$R; lstConfigFile, Current\r\n"
            "$-- BLOCK 1 / 2\r\n  #setIPSettings, DHCP\n"
            "$-- BLOCK 2 / 2\r\n  setDataInOut, IP12, auto, SBF+NMEA\n";
    const auto two = asterx::ParseConfigFileListing(blocks);
    REQUIRE(two.size() == 2u);
    CHECK(two[0].permanent);
    CHECK(two[1].command_name == "setDataInOut");

    CHECK(asterx::ParseConfigFileListing("").empty());
    CHECK(asterx::ParseConfigFileListing("$R; lcf, Current\r\n$-- BLOCK 1 / 1\r\n").empty());
}

TEST_CASE("Commands: CommandNameOfHandlesReplyMarkers") {
    CHECK(asterx::CommandNameOf("#setIPSettings, DHCP") == "setIPSettings");
    CHECK(asterx::CommandNameOf("  setElevationMask  ") == "setElevationMask");
    // The Session binds a reply to its command through this helper
    CHECK(asterx::CommandNameOf("$R: getSBFOutput, Stream1") == "getSBFOutput");
    CHECK(asterx::CommandNameOf("$R; lstConfigFile, Current") == "lstConfigFile");
    CHECK(asterx::CommandNameOf("$R: getPVTMode") == "getPVTMode");
}

TEST_CASE("Commands: DriverWhitelistMatchesDocumentedSet") {
    auto settings = MakeSettings();
    settings.nmea_streams = {{1, "COM2", {"ZDA"}, "sec1"}};
    settings.ntrip.enabled = true;
    settings.ntrip.caster = "ntrip.example.com";
    settings.ntrip.username = "user";
    settings.ntrip.password = "pw";
    settings.ntrip.mount_point = "MP1";

    auto names = asterx::DriverCommandWhitelist(asterx::BuildCommandList(settings, "IP12"));
    std::sort(names.begin(), names.end());
    std::vector<std::string> expected{
        "setAntennaType", "setAttitudeOffset", "setCN0Mask", "setClockSyncThreshold", "setDataInOut",
        "setElevationMask", "setGNSSAttitude", "setIMUOrientation", "setIMUStartupDataMode", "setINSAntLeverArm",
        "setINSNavConfig", "setMultipathMitigation", "setNMEAOutput", "setNTPServer", "setNtripSettings",
        "setNtripTlsSettings", "setPPSParameters", "setPTPServer", "setPVTMode", "setReceiverDynamics",
        "setSBFOutput", "setSatelliteHealthOverride", "setSatelliteTracking", "setSatelliteUsage",
        "setSignalTracking", "setSignalUsage", "setUserAccessLevel", "setVehicleApplication",
    };
    std::sort(expected.begin(), expected.end());
    CHECK(names == expected);
}

TEST_CASE("Commands: VerifyConfigListingAcceptsOursRejectsForeign") {
    const std::vector<std::string> whitelist{"setSBFOutput", "setUserAccessLevel"};

    std::vector<asterx::ConfigLine> ok{
        {true, "setIPSettings", "#setIPSettings, DHCP"},
        {false, "setUserAccessLevel", "setUserAccessLevel, User1, amiga, pw, User"},
        {false, "setsbfoutput", "setsbfoutput, Stream1, IP12, ExtSensorMeas, OnChange"},
    };
    CHECK_NOTHROW(asterx::VerifyConfigListing(ok, whitelist));

    // A permanent line is never "foreign", whatever its name.
    ok.push_back({true, "setEthernetMode", "#setEthernetMode, on"});
    CHECK_NOTHROW(asterx::VerifyConfigListing(ok, whitelist));

    ok.push_back({false, "setElevationMask", "setElevationMask, all, 15"});
    try {
        asterx::VerifyConfigListing(ok, whitelist);
        FAIL("foreign command accepted");
    } catch (const asterx::ConfigError &e) {
        const std::string what = e.what();
        CHECK_MESSAGE((what.find("1 foreign command") != std::string::npos), what);
        CHECK_MESSAGE((what.find("setElevationMask, all, 15") != std::string::npos), what);
    }

    // A foreign NTRIP line is reported with its password redacted.
    const std::vector<asterx::ConfigLine> ntrip{
        {false, "setNtripSettings", "setNtripSettings, NTR2, Client, host, 2101, user, SECRETPW, MP, v2, auto"},
    };
    try {
        asterx::VerifyConfigListing(ntrip, whitelist);
        FAIL("foreign command accepted");
    } catch (const asterx::ConfigError &e) {
        const std::string what = e.what();
        CHECK_MESSAGE((what.find("SECRETPW") == std::string::npos), what);
        CHECK_MESSAGE((what.find("<REDACTED>") != std::string::npos), what);
    }
}

TEST_CASE("Commands: ParsesReceiverCapabilities") {
    const std::string reply =
            "$R: getReceiverCapabilities\r\n"
            "  ReceiverCapabilities, Main+Aux1, GPSL1CA+GPSL5, COM1+IPS1,\r\n"
            "      APME+INS, 5, 100, 5\r\n";

    const auto caps = asterx::ParseReceiverCapabilitiesReply(reply);
    CHECK(caps.has_main);
    CHECK(caps.has_aux1);

    // Single-antenna receiver: parsed, but the Session refuses to continue
    const auto single = asterx::ParseReceiverCapabilitiesReply(
        "$R: getReceiverCapabilities\r\n  ReceiverCapabilities, Main, GPSL1CA, COM1, APME, 10, 100, 10\r\n");
    CHECK(single.has_main);
    CHECK_FALSE(single.has_aux1);

    CHECK_THROWS_AS((void) asterx::ParseReceiverCapabilitiesReply("$R: grc\r\n  ReceiverCapabilities, Main, 5\r\n"), asterx::ConfigError);
    CHECK_THROWS_AS((void) asterx::ParseReceiverCapabilitiesReply("$R: grc\r\n  Something, else\r\n"), asterx::ConfigError);
}

TEST_CASE("Commands: RejectsInvalidMasksAndThetaRanges") {
    auto settings = MakeSettings();
    settings.gnss.cn0_mask_dbhz = 61;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.gnss.cn0_mask_dbhz = 60;
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));

    settings.gnss.elevation_mask_deg = 91;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.gnss.elevation_mask_deg = -90;
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));

    // setIMUOrientation ranges (p.112): ThetaX/ThetaZ +-180, ThetaY +-90
    settings.imu.orientation_mode = "manual";
    settings.imu.theta_y_deg = 90.0;
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));
    settings.imu.theta_y_deg = 90.1;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.imu.theta_y_deg = 0.0;
    settings.imu.theta_x_deg = 180.5;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.imu.theta_x_deg = -180.0;
    settings.imu.theta_z_deg = -181.0;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
}

TEST_CASE("Commands: BuildsAntennaTypeCommands") {
    CHECK(asterx::BuildAntennaTypeCommand("Main", "Unknown") == "setAntennaType, Main, Unknown");
    CHECK(asterx::BuildAntennaTypeCommand("Aux1", "AERAT2775_159   SPKE") == "setAntennaType, Aux1, \"AERAT2775_159   SPKE\"");
    CHECK(asterx::BuildAntennaTypeCommand("Main", "SEPPOLANT_X_MF") == "setAntennaType, Main, SEPPOLANT_X_MF");
}

TEST_CASE("Commands: ParsesAntennaOverview") {
    const std::string single =
            "$R; lstAntennaInfo, Overview\r\n"
            "<?xml version=\"1.0\" encoding=\"ISO-8859-1\" ?>\r\n"
            "<AntennaInfo version=\"22.1.0\">\r\n"
            "  <Antenna ID=\"3COAG35         NONE\"/>\r\n"
            "  <Antenna ID=\"AERAT2775_159   SPKE\"/>\r\n"
            "  <Antenna ID=\"AS-ANT2B        NONE\"/>\r\n"
            "</AntennaInfo>\r\n";
    const auto ids = asterx::ParseAntennaOverview(single);
    REQUIRE(ids.size() == 3u);
    CHECK(ids[0] == "3COAG35         NONE");
    CHECK(ids[1] == "AERAT2775_159   SPKE");
    CHECK(ids[2] == "AS-ANT2B        NONE");

    const std::string blocks =
            "$R; lstAntennaInfo, Overview\r\n"
            "$-- BLOCK 1 / 2\r\n<AntennaInfo version=\"22.1.0\">\r\n  <Antenna ID=\"A1\"/>\n"
            "$-- BLOCK 2 / 2\r\n  <Antenna ID=\"B 2\"/>\r\n</AntennaInfo>\n";
    const auto two = asterx::ParseAntennaOverview(blocks);
    REQUIRE(two.size() == 2u);
    CHECK(two[0] == "A1");
    CHECK(two[1] == "B 2");

    CHECK(asterx::ParseAntennaOverview("").empty());
}

TEST_CASE("Commands: VerifiesAntennaTypesWithCandidates") {
    const std::vector<std::string> ids{"AS-ANT2B        NONE", "AS-ANT3B        NONE", "TRM59800.00     NONE"};
    asterx::AntennaSettings a;
    a.main_type = "Unknown";
    a.aux_type = "Unknown";
    CHECK_NOTHROW(asterx::VerifyAntennaTypes(ids, a));

    a.main_type = "AS-ANT2B        NONE";
    CHECK_NOTHROW(asterx::VerifyAntennaTypes(ids, a));

    a.main_type = "as-ant2b        none"; // case matters on the receiver (p.107)
    CHECK_THROWS_AS(asterx::VerifyAntennaTypes(ids, a), asterx::ConfigError);

    a.main_type = "AS-";
    try {
        asterx::VerifyAntennaTypes(ids, a);
        FAIL("unknown antenna accepted");
    } catch (const asterx::ConfigError &e) {
        const std::string what = e.what();
        CHECK_MESSAGE((what.find("main_antenna_type 'AS-'") != std::string::npos), what);
        CHECK_MESSAGE((what.find("closest:") != std::string::npos), what);
        CHECK_MESSAGE((what.find("AS-ANT2B") != std::string::npos), what);
        CHECK_MESSAGE((what.find("TRM59800") == std::string::npos), what);
    }

    const auto few = asterx::suggest_antenna_names(ids, "AS-ANT", 1);
    REQUIRE(few.size() == 1u);
    CHECK(few[0] == "AS-ANT2B        NONE");
}

TEST_CASE("Commands: ValidatesSatelliteAndSignalLists") {
    for (const char *ok: {"all", "none", "GPS+GALILEO", "G01+G39+R30+E36+S158+C63+J10", "g01", "gps"}) {
        CHECK_MESSAGE(asterx::IsValidSatelliteList(ok, true), ok);
    }
    CHECK_FALSE(asterx::IsValidSatelliteList("none", false));
    for (const char *bad: {"G00", "G37", "S119", "R31", "+SBAS", "-G03", "GPS++GLONASS", "GPS+", "", "GPSL1CA"}) {
        CHECK_FALSE_MESSAGE(asterx::IsValidSatelliteList(bad, true), bad);
    }

    // setSatelliteUsage stops at G32 (p.128); tracking/health go to G39 (p.96/p.127)
    CHECK(asterx::IsValidSatelliteList("G32", true, 32));
    CHECK_FALSE(asterx::IsValidSatelliteList("G33", true, 32));
    CHECK_FALSE(asterx::IsValidSatelliteList("GPS+G38", true, 32));
    CHECK(asterx::IsValidSatelliteList("GPS+G38", true, 39));

    auto settings = MakeSettings();
    settings.gnss.satellite_usage = "GPS+G38";
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(settings), asterx::ConfigError);
    settings.gnss.satellite_usage = "all";
    settings.gnss.satellite_tracking = "GPS+G38";
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(settings));

    for (const char *ok: {"all", "GPSL1CA+GALE5a", "gpsl1ca", "GPS+GALE5a"}) {
        CHECK_MESSAGE(asterx::IsValidSignalList(ok, true), ok);
    }
    CHECK(asterx::IsValidSignalList("GPSL1CA+QZSL1CB", false));
    CHECK_FALSE(asterx::IsValidSignalList("GPS", false)); // no alias for setSignalUsage
    CHECK_FALSE(asterx::IsValidSignalList("none", true));
    CHECK_FALSE(asterx::IsValidSignalList("GPSL1CA+G01", true));
}

TEST_CASE("Commands: VerifiesFirstFieldsAndReturnsPayload") {
    CHECK(asterx::verify_first_fields("$R: getReceiverDynamics\r\n  ReceiverDynamics, High\r\n",
                                          "ReceiverDynamics", {"High"}) == "High");
    CHECK_NOTHROW((void) asterx::verify_first_fields("$R: grd\r\n  ReceiverDynamics, High", "ReceiverDynamics",
                                                       {"high"}));
    CHECK_THROWS_AS((void) asterx::verify_first_fields("$R: grd\r\n  ReceiverDynamics, High", "ReceiverDynamics",
                                                    {"Low"}), asterx::ConfigError);
    CHECK(asterx::verify_first_fields("$R: ginc\r\n  INSNavConfig, on, PosStdDev+Att, POI1\r\n",
                                          "INSNavConfig", {"on", "", "POI1"}) == "on, PosStdDev+Att, POI1");
    CHECK_NOTHROW((void) asterx::verify_first_fields(
        "$R: gpm\r\n  PVTMode, Rover, StandAlone+SBAS+DGNSS+RTKFloat+RTKFixed, auto", "PVTMode", {"Rover"}));
    CHECK_THROWS_AS((void) asterx::verify_first_fields("$R: gpm\r\n  Other, x", "PVTMode", {"Rover"}), asterx::ConfigError);
    CHECK_THROWS_AS((void) asterx::verify_first_fields("$R: ginc\r\n  INSNavConfig, on", "INSNavConfig",
                                                    {"on", "", "POI1"}), asterx::ConfigError);
    // Quoted echoes (antenna names with spaces) are unquoted before comparing
    CHECK_NOTHROW((void) asterx::verify_first_fields(
        "$R: getAntennaType, Main\r\n  AntennaType, Main, \"AERAT2775_159   SPKE\"\r\n", "AntennaType",
        {"Main", "AERAT2775_159   SPKE"}));
    // An empty expectation list is a presence check (expanded signal lists)
    CHECK(asterx::verify_first_fields("$R: gnt\r\n  SignalTracking, GPSL1CA+GPSL2C\r\n", "SignalTracking", {}) == "GPSL1CA+GPSL2C");
    // getCN0Mask answers one line per signal; the first is the compared one
    CHECK(asterx::verify_first_fields(
                  "$R: gcm\r\n  CN0Mask, GPSL1CA, 0\r\n  CN0Mask, Reserved2, 1\r\n", "CN0Mask", {"", "0"}) == "GPSL1CA, 0");
}

TEST_CASE("Commands: BuildsPpsCommand") {
    asterx::PpsSettings pps;
    CHECK(asterx::BuildPpsCommand(pps) == "setPPSParameters, sec1, Low2High, 0.00, GPS, 60, 5.000000");
    pps.delay_ns = 23.4;
    pps.polarity = "High2Low";
    pps.time_scale = "UTC";
    pps.pulse_width_ms = 0.1;
    CHECK(asterx::BuildPpsCommand(pps) == "setPPSParameters, sec1, High2Low, 23.40, UTC, 60, 0.100000");
    // The receiver echoes exactly what it parsed, so the sent text is the expectation (p.157)
    CHECK_NOTHROW((void) asterx::verify_first_fields(
        "$R: spps\r\n  PPSParameters, sec1, High2Low, 23.40, UTC, 60, 0.100000\r\n", "PPSParameters",
        {"sec1", "High2Low", "23.40", "UTC", "60", "0.100000"}));

    CHECK(asterx::FmtCommandDecimal(0.000001, 6) == "0.000001");
    CHECK(asterx::FmtCommandDecimal(-0.0004, 3) == "0.000");
}

TEST_CASE("Commands: RejectsPpsOutOfRange") {
    auto s = MakeSettings();
    s.time_system.pps.max_holdover_s = 3601;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError);
    s.time_system.pps.max_holdover_s = 3600;
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(s));

    s.time_system.pps.pulse_width_ms = 0.0;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError);
    s.time_system.pps.pulse_width_ms = 1000.0;
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(s));

    s.time_system.pps.delay_ns = 1000000.5;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError);
    s.time_system.pps.delay_ns = 0.0;

    s.time_system.pps.polarity = "Low2Hight";
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError);
    s.time_system.pps.polarity = "low2high"; // spelling is case-insensitive
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(s));
}

TEST_CASE("Commands: BuildsNtripCommands") {
    asterx::NtripSettings n;
    CHECK(asterx::BuildNtripCommands(n) == std::vector<std::string>{"setNtripSettings, NTR1, off"});

    n.enabled = true;
    n.caster = "www.smartnetaus.com";
    n.port = 15101;
    n.username = "user";
    n.password = "p&w,1";
    n.mount_point = "MSM_VRS";
    n.send_gga = "auto";
    const auto cmds = asterx::BuildNtripCommands(n);
    REQUIRE(cmds.size() == 2u);
    CHECK(cmds[0] == "setNtripSettings, NTR1, Client, www.smartnetaus.com, 15101, user, p%%AMw%%CM1, MSM_VRS, v2, auto");
    CHECK(cmds[1] == "setNtripTlsSettings, NTR1, off, \"\"");
    n.tls = true;
    CHECK(asterx::BuildNtripCommands(n)[1] == "setNtripTlsSettings, NTR1, on, \"\"");
    n.tls = false;
    n.password = "a b";
    CHECK(asterx::BuildNtripCommands(n)[0].find(", \"a b\", ") != std::string::npos);

    CHECK(asterx::EscapeNtripPassword("a\"b'c$d&e,f") == "a%%DQb%%SQc%%DLd%%AMe%%CMf");
}

TEST_CASE("Commands: NtripReadbackComparesEverythingButThePassword") {
    auto s = MakeSettings();
    s.ntrip.enabled = true;
    s.ntrip.caster = "ntrip.example.com";
    s.ntrip.port = 2101;
    s.ntrip.username = "USER";
    s.ntrip.password = "PWD";
    s.ntrip.mount_point = "MP1";

    const auto cmds = asterx::BuildCommandList(s, "IP12");
    const auto get = IndexOf(cmds, "getNtripSettings, NTR1");
    REQUIRE(get < cmds.size());
    // Field order of p.185: Cd, Mode, Caster, Port, UserName, Password, MountPoint, Version, SendGGA.
    // Only the password is encrypted by the receiver, so only it is skipped.
    CHECK(cmds[get].verify_fields == (std::vector<std::string>{"NTR1", "Client", "ntrip.example.com", "2101", "USER", "",
                                        "MP1", "v2", "auto"}));
    // The documented echo satisfies the expectation
    CHECK_NOTHROW((void) asterx::verify_first_fields(
        "$R: snts\r\n  NtripSettings, NTR1, Client, ntrip.example.com, 2101, USER, DD3BQT8GBF, MP1, v2, auto\r\n",
        "NtripSettings", cmds[get].verify_fields));
    // A caster the receiver did not store is caught
    CHECK_THROWS_AS((void) asterx::verify_first_fields(
                     "$R: snts\r\n  NtripSettings, NTR1, Client, other.host, 2101, USER, DD3BQT8GBF, MP1, v2, auto\r\n",
                     "NtripSettings", cmds[get].verify_fields), asterx::ConfigError);
}

TEST_CASE("Commands: NtripValidationOnlyWhenEnabled") {
    auto s = MakeSettings();
    s.ntrip.caster = "www.example.com";
    s.ntrip.username = "<?>";
    s.ntrip.password = "<?>";
    s.ntrip.mount_point = "MP";
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(s)); // disabled: placeholders are fine

    s.ntrip.enabled = true;
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError);
    s.ntrip.username = "user";
    s.ntrip.password = "pw";
    CHECK_NOTHROW(asterx::ValidateReceiverSettings(s));
    s.ntrip.version = "v3";
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError);
    s.ntrip.version = "v2";
    s.ntrip.send_gga = "sec2";
    CHECK_THROWS_AS(asterx::ValidateReceiverSettings(s), asterx::ConfigError);
}

TEST_CASE("Commands: NtripPasswordEscapeTable") {
    // p.64: " ' $ & , -> %%DQ %%SQ %%DL %%AM %%CM
    CHECK(asterx::EscapeNtripPassword("p\"a'b$c&d,e") == "p%%DQa%%SQb%%DLc%%AMd%%CMe");
    CHECK(asterx::EscapeNtripPassword("plain") == "plain");
}

TEST_CASE("Commands: RedactHidesOnlyTheNtripPassword") {
    const std::string full = "setNtripSettings, NTR1, Client, caster, 2101, user, secret, MP, v2, auto";
    const auto redacted = asterx::RedactCmd(full);
    CHECK(redacted.find("secret") == std::string::npos);
    CHECK(redacted.find("<REDACTED>") != std::string::npos);
    CHECK(redacted.find("user") != std::string::npos);
    CHECK(asterx::RedactCmd("$R: setNtripSettings, NTR1, off").find("NTR1, off") != std::string::npos);
    CHECK(asterx::RedactCmd("setPVTMode, Rover, all") == "setPVTMode, Rover, all");
}
