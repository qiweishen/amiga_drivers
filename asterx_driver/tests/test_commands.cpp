// SPDX-License-Identifier: BSD-3-Clause
#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "commands.hpp"

namespace {

bool stream_contains(const std::vector<asterx::SbfStream>& streams,
                     const std::string& block) {
    for (const auto& stream: streams) {
        if (std::find(stream.blocks.begin(), stream.blocks.end(), block) != stream.blocks.end()) {
            return true;
        }
    }
    return false;
}

std::size_t count_kind(const std::vector<asterx::Command>& cmds,
                       asterx::CommandKind kind) {
    return static_cast<std::size_t>(
        std::count_if(cmds.begin(), cmds.end(),
                      [kind](const asterx::Command& c) { return c.kind == kind; }));
}

std::size_t index_of(const std::vector<asterx::Command>& cmds,
                     const std::string& text_prefix) {
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].text.rfind(text_prefix, 0) == 0) {
            return i;
        }
    }
    return cmds.size();
}

}  // namespace

TEST(Commands, DefaultStreamsUseOnChangeAndRequiredBlocks) {
    asterx::ReceiverSettings settings;

    ASSERT_EQ(settings.streams.size(), 6u);
    for (const auto& stream: settings.streams) {
        EXPECT_EQ(stream.interval, "OnChange");
    }

    EXPECT_TRUE(stream_contains(settings.streams, "ExtSensorMeas"));
    EXPECT_TRUE(stream_contains(settings.streams, "MeasEpoch"));
    EXPECT_TRUE(stream_contains(settings.streams, "MeasExtra"));
    EXPECT_TRUE(stream_contains(settings.streams, "AuxAntPositions"));
    EXPECT_TRUE(stream_contains(settings.streams, "AttEuler"));
    EXPECT_TRUE(stream_contains(settings.streams, "IMUSetup"));
    EXPECT_TRUE(stream_contains(settings.streams, "ExtSensorStatus"));
    EXPECT_TRUE(stream_contains(settings.streams, "ReceiverStatus"));
    EXPECT_TRUE(stream_contains(settings.streams, "ChannelStatus"));
    EXPECT_TRUE(stream_contains(settings.streams, "GPS"));
    EXPECT_TRUE(stream_contains(settings.streams, "GLO"));
    EXPECT_TRUE(stream_contains(settings.streams, "GAL"));
    EXPECT_TRUE(stream_contains(settings.streams, "BDS"));
    EXPECT_TRUE(stream_contains(settings.streams, "QZS"));
    EXPECT_TRUE(stream_contains(settings.streams, "RawNavBits"));
    EXPECT_TRUE(stream_contains(settings.streams, "ReceiverSetup"));
    EXPECT_TRUE(stream_contains(settings.streams, "DiffCorr"));
    EXPECT_TRUE(settings.configure_all_tracking);
    EXPECT_EQ(settings.cn0_mask_dbhz, 0);
}

TEST(Commands, ValidationRequiresLeverArm) {
    asterx::ReceiverSettings settings;
    EXPECT_THROW(asterx::validate_receiver_settings(settings), asterx::ConfigError);

    settings.ant_lever_arm_configured = true;
    settings.ant_lever_arm_m = asterx::Vec3{0.1, -0.2, 0.3};
    EXPECT_NO_THROW(asterx::validate_receiver_settings(settings));
}

TEST(Commands, BuildsLongSbfOutputWithoutTruncation) {
    asterx::SbfStream stream;
    stream.stream_id = 6;
    stream.interval = "OnChange";
    for (int i = 0; i < 50; ++i) {
        stream.blocks.push_back("Block" + std::to_string(i));
    }

    const auto cmd = asterx::build_sbf_output_command(stream, "IP10");
    EXPECT_GT(cmd.size(), 256u);
    EXPECT_LT(cmd.size(), 2000u);
    EXPECT_NE(cmd.find("setSBFOutput, Stream6, IP10, "), std::string::npos);
    EXPECT_NE(cmd.find("Block0+Block1"), std::string::npos);
    EXPECT_NE(cmd.find("Block49"), std::string::npos);
}

TEST(Commands, BuildsLeverArmWithReceiverPrecision) {
    EXPECT_EQ(asterx::build_ins_ant_lever_arm_command(asterx::Vec3{0.0, 0.0, 0.0}),
              "setINSAntLeverArm, 0.000, 0.000, 0.000");
    EXPECT_EQ(asterx::build_ins_ant_lever_arm_command(asterx::Vec3{-0.0001, 1.2344, -2.3456}),
              "setINSAntLeverArm, 0.000, 1.234, -2.346");
}

TEST(Commands, RejectsOversizedSbfOutputCommand) {
    asterx::SbfStream stream;
    stream.stream_id = 1;
    stream.interval = "OnChange";
    for (int i = 0; i < 250; ++i) {
        stream.blocks.push_back("VeryLongSyntheticBlockName" + std::to_string(i));
    }

    EXPECT_THROW((void) asterx::build_sbf_output_command(stream, "IP10"),
                 asterx::ConfigError);
}

TEST(Commands, RejectsInvalidDescriptor) {
    asterx::SbfStream stream;
    stream.stream_id = 1;
    stream.interval = "OnChange";
    stream.blocks = {"ExtSensorMeas"};

    EXPECT_THROW((void) asterx::build_sbf_output_command(stream, ""),
                 asterx::ConfigError);
    EXPECT_THROW((void) asterx::build_sbf_output_command(stream, "IP10, none"),
                 asterx::ConfigError);

    asterx::ReceiverSettings settings;
    settings.ant_lever_arm_configured = true;
    EXPECT_THROW((void) asterx::build_command_list(settings, ""),
                 asterx::ConfigError);
}

TEST(Commands, RejectsInvalidTrackingCn0Mask) {
    asterx::ReceiverSettings settings;
    settings.ant_lever_arm_configured = true;
    settings.cn0_mask_dbhz = 61;

    EXPECT_THROW(asterx::validate_receiver_settings(settings),
                 asterx::ConfigError);
}

TEST(Commands, CommandListFollowsConfigureSequence) {
    asterx::ReceiverSettings settings;
    settings.user = "admin";
    settings.password = "secret";
    settings.ant_lever_arm_configured = true;
    settings.ant_lever_arm_m = asterx::Vec3{0.1, -0.2, 0.3};

    const auto cmds = asterx::build_command_list(settings, "IP12");

    ASSERT_FALSE(cmds.empty());
    // login always first, with real credentials.
    EXPECT_EQ(cmds[0].text, "login, admin, secret");
    EXPECT_EQ(cmds[0].kind, asterx::CommandKind::Plain);

    // Exactly one capabilities check, ten tolerated stream wipes, and one
    // geometry readback of each kind.
    EXPECT_EQ(count_kind(cmds, asterx::CommandKind::CheckCapabilities), 1u);
    EXPECT_EQ(count_kind(cmds, asterx::CommandKind::ToleratedError), 10u);
    EXPECT_EQ(count_kind(cmds, asterx::CommandKind::VerifyImuOrientation), 1u);
    EXPECT_EQ(count_kind(cmds, asterx::CommandKind::VerifyLeverArm), 1u);
    EXPECT_EQ(count_kind(cmds, asterx::CommandKind::VerifyGnssAttitude), 1u);
    EXPECT_EQ(count_kind(cmds, asterx::CommandKind::VerifyAttitudeOffset), 1u);

    // SBF output is enabled on, and streams are targeted at, our own
    // connection descriptor.
    EXPECT_LT(index_of(cmds, "setDataInOut, IP12, , +SBF"), cmds.size());
    std::size_t sbf_streams = 0;
    for (const auto& c : cmds) {
        if (c.text.rfind("setSBFOutput, Stream", 0) == 0 &&
            c.text.find(", IP12, ") != std::string::npos) {
            ++sbf_streams;
        }
    }
    EXPECT_EQ(sbf_streams, settings.streams.size());

    // Each set is followed (eventually) by its readback.
    EXPECT_LT(index_of(cmds, "setIMUOrientation"), index_of(cmds, "getIMUOrientation"));
    EXPECT_LT(index_of(cmds, "setINSAntLeverArm"), index_of(cmds, "getINSAntLeverArm"));
    EXPECT_LT(index_of(cmds, "setGNSSAttitude"), index_of(cmds, "getGNSSAttitude"));
    EXPECT_LT(index_of(cmds, "setAttitudeOffset"), index_of(cmds, "getAttitudeOffset"));

    // Stream setup comes after the geometry verification.
    EXPECT_LT(index_of(cmds, "getAttitudeOffset"), index_of(cmds, "setSBFOutput, Stream1, IP12"));
}

TEST(Commands, RedactsLoginForLogging) {
    EXPECT_EQ(asterx::redact_cmd("login, admin, secret"),
              "login, <REDACTED>, <REDACTED>");
    EXPECT_EQ(asterx::redact_cmd("setDataInOut, IP10, , +SBF"),
              "setDataInOut, IP10, , +SBF");
}

TEST(Commands, ParsesReceiverCapabilities) {
    const std::string reply =
        "$R: grc\r\n"
        "  ReceiverCapabilities, Main+Aux1, GPSL1CA+GPSL5, COM1+IPS1,\r\n"
        "      APME+INS, 5, 100, 5\r\n"
        "COM1>";

    const auto caps = asterx::parse_receiver_capabilities_reply(reply);
    EXPECT_TRUE(caps.has_main);
    EXPECT_TRUE(caps.has_aux1);
    EXPECT_EQ(caps.measurement_interval_ms, 5);
    EXPECT_EQ(caps.pvt_interval_ms, 100);
    EXPECT_EQ(caps.ins_interval_ms, 5);
}

TEST(Commands, VerifiesImuOrientationReply) {
    asterx::ReceiverSettings settings;
    settings.imu_orientation_mode = "manual";
    settings.theta_x_deg = -90.0;
    settings.theta_y_deg = 0.0;
    settings.theta_z_deg = 1.5;

    const std::string ok =
        "$R: gio\r\n"
        "  IMUOrientation, manual, -90.000, 0.000, 1.500\r\n"
        "COM1>";
    EXPECT_NO_THROW(asterx::verify_imu_orientation_reply(ok, settings));

    const std::string bad =
        "$R: gio\r\n"
        "  IMUOrientation, fixed, -90.000, 0.000, 1.500\r\n"
        "COM1>";
    EXPECT_THROW(asterx::verify_imu_orientation_reply(bad, settings),
                 asterx::ConfigError);
}

TEST(Commands, VerifiesLeverArmAndAttitudeReplies) {
    // SsnRx delivers replies without the trailing prompt; the verifiers must
    // accept both the prompt-terminated and the bare form.
    EXPECT_NO_THROW(asterx::verify_ins_ant_lever_arm_reply(
        "$R: gial\r\n  INSAntLeverArm, 0.100, -0.200, 0.300\r\nCOM1>",
        asterx::Vec3{0.1, -0.2, 0.3}));
    EXPECT_NO_THROW(asterx::verify_ins_ant_lever_arm_reply(
        "$R: gial\r\n  INSAntLeverArm, 0.100, -0.200, 0.300",
        asterx::Vec3{0.1, -0.2, 0.3}));

    EXPECT_NO_THROW(asterx::verify_gnss_attitude_reply(
        "$R: gga\r\n  GNSSAttitude, MultiAntenna\r\nCOM1>",
        "MultiAntenna"));

    EXPECT_NO_THROW(asterx::verify_attitude_offset_reply(
        "$R: gto\r\n  AttitudeOffset, 3.000, -1.500\r\nCOM1>",
        asterx::AttitudeOffset{3.0, -1.5}));
    EXPECT_NO_THROW(asterx::verify_attitude_offset_reply(
        "$R: gto\r\n  AttitudeOffset, 3.000, -1.500",
        asterx::AttitudeOffset{3.0, -1.5}));
}
