// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace asterx {
    class ConfigError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // One SBF stream definition pushed to the receiver. Each stream lists a set
    // of SBF blocks and a single MsgInterval token (e.g. "msec5", "msec100", "sec1").
    struct SbfStream {
        int stream_id; // 1..10 — receiver-side Stream<id>
        std::vector<std::string> blocks; // e.g. {"ExtSensorMeas"}
        std::string interval; // e.g. "OnChange"
    };

    struct Vec3 {
        double x{0.0};
        double y{0.0};
        double z{0.0};
    };

    struct AttitudeOffset {
        double heading_deg{0.0};
        double pitch_deg{0.0};
    };

    struct ReceiverCapabilities {
        bool has_main{false};
        bool has_aux1{false};
        int measurement_interval_ms{0};
        int pvt_interval_ms{0};
        int ins_interval_ms{0};
    };

    struct ReceiverSettings {
        // Login
        std::string user{"admin"};
        std::string password{"septentrio"};

        // SBF stream layout. EndOfMeas / EndOfAtt are epoch terminators that help
        // downstream parsers know when a measurement / attitude epoch is complete.
        std::vector<SbfStream> streams{
            {1, {"ExtSensorMeas"}, "OnChange"},
            {2, {"MeasEpoch", "MeasExtra", "EndOfMeas"}, "OnChange"},
            {3, {"AuxAntPositions", "AttEuler", "AttCovEuler", "EndOfAtt"}, "OnChange"},
            {4, {"ExtSensorStatus", "ExtSensorInfo", "IMUSetup"}, "OnChange"},
            {5, {"ReceiverStatus", "QualityInd", "ReceiverTime", "ChannelStatus"}, "OnChange"},
            {
                6, {
                    "GPS", "GLO", "GAL", "BDS", "QZS", "RawNavBits", "ReceiverSetup",
                    "Commands", "DiffCorr"
                },
                "OnChange"
            },
        };

        // Legacy config compatibility: load_config maps this to imu_orientation_mode.
        bool use_sensor_default{true};

        // IMU setup. OrientationMode is SensorDefault, manual, or fixed.
        std::string imu_startup_data_mode{"Boot"};
        std::string imu_orientation_mode{"SensorDefault"};
        double theta_x_deg{0.0};
        double theta_y_deg{0.0};
        double theta_z_deg{0.0};
        Vec3 ant_lever_arm_m{};
        bool ant_lever_arm_configured{false};

        // Dual-antenna GNSS attitude.
        bool require_aux1{true};
        std::string gnss_attitude_mode{"MultiAntenna"};
        AttitudeOffset attitude_offset_deg{};

        // Reset tracking-related filters so collection is not limited by stale
        // receiver settings from a previous session.
        bool configure_all_tracking{true};
        int cn0_mask_dbhz{0};
    };

    // How the Session must treat the reply to one command in the configure
    // sequence.
    enum class CommandKind {
        Plain, // error reply -> configuration failure
        ToleratedError, // error reply logged and ignored (stream wipes)
        CheckCapabilities, // parse ReceiverCapabilities; fail if Aux1 required but absent
        VerifyImuOrientation, // reply must echo the requested IMU orientation
        VerifyLeverArm, // reply must echo the requested INS lever arm
        VerifyGnssAttitude, // reply must echo the requested attitude mode
        VerifyAttitudeOffset, // reply must echo the requested attitude offset
    };

    struct Command {
        std::string text;
        CommandKind kind{CommandKind::Plain};
    };

    [[nodiscard]] bool is_valid_sbf_interval(const std::string &interval);

    // descriptor is the receiver-side connection descriptor of OUR OWN session
    // (e.g. "IP10"), as reported by SsnRx::getConnectionDescriptor(). SBF output
    // is targeted at it, so the stream dies with the connection.
    [[nodiscard]] std::string build_sbf_output_command(const SbfStream &stream,
                                                       const std::string &descriptor);

    [[nodiscard]] std::string build_ins_ant_lever_arm_command(Vec3 lever_arm_m);

    [[nodiscard]] std::string build_imu_orientation_command(const ReceiverSettings &settings);

    // Full ordered configure sequence: login -> capabilities -> stream wipe ->
    // SBF output enable -> tracking -> IMU/lever-arm/attitude (+ readbacks) ->
    // setSBFOutput per stream. Re-run verbatim after every reconnect.
    [[nodiscard]] std::vector<Command> build_command_list(const ReceiverSettings &settings,
                                                          const std::string &descriptor);

    [[nodiscard]] ReceiverCapabilities parse_receiver_capabilities_reply(const std::string &reply);

    void validate_receiver_settings(const ReceiverSettings &settings);

    void verify_imu_orientation_reply(const std::string &reply, const ReceiverSettings &settings);

    void verify_ins_ant_lever_arm_reply(const std::string &reply, Vec3 expected);

    void verify_gnss_attitude_reply(const std::string &reply, const std::string &expected_mode);

    void verify_attitude_offset_reply(const std::string &reply, AttitudeOffset expected);

    // Redact "login, user, password" lines before logging.
    [[nodiscard]] std::string redact_cmd(const std::string &cmd);
} // namespace asterx
