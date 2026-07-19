// SPDX-License-Identifier: BSD-3-Clause
//
// Builds the small subset of the Septentrio ASCII command interface that the
// slim driver needs, and verifies the get* readbacks of the geometry
// parameters (IMU orientation, INS lever arm, GNSS attitude + offset).
//
// Transport, prompt handling and reply/error classification live in the
// Septentrio SsnRx SDK (3rd_party/RxTools/ssnrx); everything here is pure
// string logic so it stays unit-testable.
//
// Reference: docs/AsteRx-i3 D Pro+ Firmware v1.5.2 Reference Guide.pdf
//
#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace asterx {
    namespace {
        constexpr std::size_t kMaxAsciiCommandLength = 2000;

        std::string join(const std::vector<std::string> &parts, char sep) {
            std::string out;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i) out.push_back(sep);
                out.append(parts[i]);
            }
            return out;
        }

        std::string trim(std::string s) {
            auto not_space = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
            s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
            return s;
        }

        std::string lower_copy(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool equals_ci(const std::string &a, const std::string &b) {
            return lower_copy(a) == lower_copy(b);
        }

        bool starts_with_ci(const std::string &s, const std::string &prefix) {
            if (s.size() < prefix.size()) return false;
            return lower_copy(s.substr(0, prefix.size())) == lower_copy(prefix);
        }

        std::vector<std::string> split(const std::string &s, char sep) {
            std::vector<std::string> out;
            std::stringstream ss(s);
            std::string part;
            while (std::getline(ss, part, sep)) {
                out.push_back(trim(part));
            }
            return out;
        }

        std::string fmt_command_decimal(double v) {
            if (std::fabs(v) < 0.0005) {
                v = 0.0;
            }
            std::ostringstream os;
            os << std::fixed << std::setprecision(3) << v;
            return os.str();
        }

        int parse_int_prefix(const std::string &s, const std::string &field) {
            std::istringstream is(trim(s));
            int v = 0;
            if (!(is >> v)) {
                throw ConfigError("could not parse integer field '" + field + "' from '" + s + "'");
            }
            return v;
        }

        double parse_double_prefix(const std::string &s, const std::string &field) {
            std::istringstream is(trim(s));
            double v = 0.0;
            if (!(is >> v)) {
                throw ConfigError("could not parse numeric field '" + field + "' from '" + s + "'");
            }
            return v;
        }

        bool close_enough(double a, double b) {
            return std::fabs(a - b) <= 0.001;
        }

        std::string find_config_line_payload(const std::string &reply, const std::string &key) {
            std::istringstream lines(reply);
            std::string line;
            const std::string prefix = key + ",";
            while (std::getline(lines, line)) {
                line = trim(line);
                if (starts_with_ci(line, prefix)) {
                    return trim(line.substr(prefix.size()));
                }
            }
            throw ConfigError("receiver reply did not contain '" + key + "' line: " + reply);
        }

        std::string compact_payload_after_key(const std::string &reply, const std::string &key) {
            const std::string marker = key + ",";
            const auto pos = reply.find(marker);
            if (pos == std::string::npos) {
                throw ConfigError("receiver reply did not contain '" + key + "' payload: " + reply);
            }

            std::string payload = reply.substr(pos + marker.size());
            const auto prompt = payload.find('>');
            if (prompt != std::string::npos) {
                payload = payload.substr(0, prompt);
            }
            for (char &c: payload) {
                if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            }
            return payload;
        }

        void enforce_command_length(const std::string &cmd) {
            if (cmd.size() > kMaxAsciiCommandLength) {
                throw ConfigError("ASCII command exceeds Septentrio 2000-character limit");
            }
        }

        void enforce_descriptor(const std::string &descriptor) {
            if (descriptor.empty() ||
                descriptor.find(',') != std::string::npos ||
                descriptor.find(' ') != std::string::npos) {
                throw ConfigError("invalid connection descriptor '" + descriptor + "'");
            }
        }

        std::string set_vec3_command(const std::string &name, Vec3 v) {
            return name + ", " + fmt_command_decimal(v.x) + ", " +
                   fmt_command_decimal(v.y) + ", " + fmt_command_decimal(v.z);
        }
    } // namespace

    bool is_valid_sbf_interval(const std::string &interval) {
        static const std::unordered_set<std::string> allowed{
            "onchange",
            "off",
            "msec5", "msec10", "msec20", "msec40", "msec50",
            "msec100", "msec200", "msec500",
            "sec1", "sec2", "sec5", "sec10", "sec15", "sec30", "sec60",
            "min2", "min5", "min10", "min15", "min30", "min60",
        };
        return allowed.find(lower_copy(interval)) != allowed.end();
    }

    std::string build_sbf_output_command(const SbfStream &stream,
                                         const std::string &descriptor) {
        if (stream.stream_id < 1 || stream.stream_id > 10) {
            throw ConfigError("SBF stream id must be in range 1..10");
        }
        enforce_descriptor(descriptor);
        if (stream.blocks.empty()) {
            throw ConfigError("SBF stream must contain at least one block");
        }
        if (!is_valid_sbf_interval(stream.interval)) {
            throw ConfigError("unsupported SBF interval '" + stream.interval + "'");
        }

        const std::string cmd =
                "setSBFOutput, Stream" + std::to_string(stream.stream_id) +
                ", " + descriptor +
                ", " + join(stream.blocks, '+') +
                ", " + stream.interval;
        enforce_command_length(cmd);
        return cmd;
    }

    std::string build_ins_ant_lever_arm_command(Vec3 lever_arm_m) {
        return set_vec3_command("setINSAntLeverArm", lever_arm_m);
    }

    std::string build_imu_orientation_command(const ReceiverSettings &settings) {
        if (equals_ci(settings.imu_orientation_mode, "SensorDefault")) {
            return "setIMUOrientation, SensorDefault";
        }
        return "setIMUOrientation, " + settings.imu_orientation_mode + ", " +
               fmt_command_decimal(settings.theta_x_deg) + ", " +
               fmt_command_decimal(settings.theta_y_deg) + ", " +
               fmt_command_decimal(settings.theta_z_deg);
    }

    std::vector<Command> build_command_list(const ReceiverSettings &settings,
                                            const std::string &descriptor) {
        enforce_descriptor(descriptor);

        std::vector<Command> cmds;
        cmds.reserve(32);

        // 1) Authenticate. SsnRx classifies the "$R?" reply, so a refused login
        //    surfaces as a plain command error.
        cmds.push_back({
            "login, " + settings.user + ", " + settings.password,
            CommandKind::Plain
        });

        // 2) Check capabilities before configuring dual-antenna collection.
        cmds.push_back({"getReceiverCapabilities", CommandKind::CheckCapabilities});

        // 3) Wipe all persistent SBF output streams so stale streams (e.g. left
        //    behind by an RxControl session) do not duplicate epochs or keep old
        //    rates alive. The receiver may answer "not configured" — tolerated.
        for (int stream_id = 1; stream_id <= 10; ++stream_id) {
            cmds.push_back({
                "setSBFOutput, Stream" + std::to_string(stream_id) +
                ", none, none, off",
                CommandKind::ToleratedError
            });
        }

        // 4) Enable SBF output on our own connection. The empty middle field
        //    leaves the input direction (command entry) unchanged.
        cmds.push_back({"setDataInOut, " + descriptor + ", , +SBF", CommandKind::Plain});

        // 5) Reset tracking/usage filters so all supported observables can be
        //    generated. setSignalTracking restarts tracking loops, so do this
        //    before enabling output streams.
        if (settings.configure_all_tracking) {
            cmds.push_back({"setSatelliteTracking, all", CommandKind::Plain});
            cmds.push_back({"setSignalTracking, all", CommandKind::Plain});
            cmds.push_back({"setSignalUsage, all, all", CommandKind::Plain});
            cmds.push_back({
                "setCN0Mask, all, " + std::to_string(settings.cn0_mask_dbhz),
                CommandKind::Plain
            });
        }

        // 6) IMU startup mode, orientation, and antenna lever arm — each set is
        //    read back and verified: a silently wrong geometry parameter corrupts
        //    every dataset recorded with it.
        cmds.push_back({
            "setIMUStartupDataMode, " + settings.imu_startup_data_mode,
            CommandKind::Plain
        });
        cmds.push_back({build_imu_orientation_command(settings), CommandKind::Plain});
        cmds.push_back({"getIMUOrientation", CommandKind::VerifyImuOrientation});

        cmds.push_back({
            build_ins_ant_lever_arm_command(settings.ant_lever_arm_m),
            CommandKind::Plain
        });
        cmds.push_back({"getINSAntLeverArm", CommandKind::VerifyLeverArm});

        // 7) Dual-antenna GNSS attitude setup and offset confirmation.
        cmds.push_back({
            "setGNSSAttitude, " + settings.gnss_attitude_mode,
            CommandKind::Plain
        });
        cmds.push_back({"getGNSSAttitude", CommandKind::VerifyGnssAttitude});

        cmds.push_back({
            "setAttitudeOffset, " +
            fmt_command_decimal(settings.attitude_offset_deg.heading_deg) +
            ", " +
            fmt_command_decimal(settings.attitude_offset_deg.pitch_deg),
            CommandKind::Plain
        });
        cmds.push_back({"getAttitudeOffset", CommandKind::VerifyAttitudeOffset});

        // 8) SBF stream membership + intervals, targeted at our own connection.
        for (const auto &s: settings.streams) {
            if (s.blocks.empty()) continue;
            cmds.push_back({build_sbf_output_command(s, descriptor), CommandKind::Plain});
        }

        return cmds;
    }

    ReceiverCapabilities parse_receiver_capabilities_reply(const std::string &reply) {
        const std::string payload = compact_payload_after_key(reply, "ReceiverCapabilities");
        const auto fields = split(payload, ',');
        if (fields.size() < 7) {
            throw ConfigError("ReceiverCapabilities reply had too few fields: " + reply);
        }

        ReceiverCapabilities caps;
        for (const auto &antenna: split(fields[0], '+')) {
            if (equals_ci(antenna, "Main")) caps.has_main = true;
            if (equals_ci(antenna, "Aux1")) caps.has_aux1 = true;
        }

        caps.measurement_interval_ms = parse_int_prefix(fields[fields.size() - 3], "measurement_interval_ms");
        caps.pvt_interval_ms = parse_int_prefix(fields[fields.size() - 2], "pvt_interval_ms");
        caps.ins_interval_ms = parse_int_prefix(fields[fields.size() - 1], "ins_interval_ms");
        return caps;
    }

    void verify_imu_orientation_reply(const std::string &reply, const ReceiverSettings &settings) {
        const auto fields = split(find_config_line_payload(reply, "IMUOrientation"), ',');
        if (fields.empty()) {
            throw ConfigError("IMUOrientation reply did not contain an orientation mode");
        }
        if (!equals_ci(fields[0], settings.imu_orientation_mode)) {
            throw ConfigError("IMU orientation mismatch: expected " +
                              settings.imu_orientation_mode + ", got " + fields[0]);
        }
        if (!equals_ci(settings.imu_orientation_mode, "SensorDefault")) {
            if (fields.size() < 4) {
                throw ConfigError("IMUOrientation reply did not include theta values");
            }
            const double theta_x = parse_double_prefix(fields[1], "ThetaX");
            const double theta_y = parse_double_prefix(fields[2], "ThetaY");
            const double theta_z = parse_double_prefix(fields[3], "ThetaZ");
            if (!close_enough(theta_x, settings.theta_x_deg) ||
                !close_enough(theta_y, settings.theta_y_deg) ||
                !close_enough(theta_z, settings.theta_z_deg)) {
                throw ConfigError("IMU orientation theta values do not match requested config");
            }
        }
    }

    void verify_ins_ant_lever_arm_reply(const std::string &reply, Vec3 expected) {
        const auto fields = split(find_config_line_payload(reply, "INSAntLeverArm"), ',');
        if (fields.size() < 3) {
            throw ConfigError("INSAntLeverArm reply did not contain x/y/z values");
        }
        const Vec3 actual{
            parse_double_prefix(fields[0], "INSAntLeverArm.X"),
            parse_double_prefix(fields[1], "INSAntLeverArm.Y"),
            parse_double_prefix(fields[2], "INSAntLeverArm.Z"),
        };
        if (!close_enough(actual.x, expected.x) ||
            !close_enough(actual.y, expected.y) ||
            !close_enough(actual.z, expected.z)) {
            throw ConfigError("INS antenna lever arm does not match requested config");
        }
    }

    void verify_gnss_attitude_reply(const std::string &reply, const std::string &expected_mode) {
        const auto fields = split(find_config_line_payload(reply, "GNSSAttitude"), ',');
        if (fields.empty()) {
            throw ConfigError("GNSSAttitude reply did not contain a mode");
        }
        if (!equals_ci(fields[0], expected_mode)) {
            throw ConfigError("GNSS attitude mode mismatch: expected " +
                              expected_mode + ", got " + fields[0]);
        }
    }

    void verify_attitude_offset_reply(const std::string &reply, AttitudeOffset expected) {
        const auto fields = split(find_config_line_payload(reply, "AttitudeOffset"), ',');
        if (fields.size() < 2) {
            throw ConfigError("AttitudeOffset reply did not contain heading/pitch values");
        }
        const double heading = parse_double_prefix(fields[0], "AttitudeOffset.Heading");
        const double pitch = parse_double_prefix(fields[1], "AttitudeOffset.Pitch");
        if (!close_enough(heading, expected.heading_deg) ||
            !close_enough(pitch, expected.pitch_deg)) {
            throw ConfigError("GNSS attitude offset does not match requested config");
        }
    }

    void validate_receiver_settings(const ReceiverSettings &settings) {
        if (!equals_ci(settings.imu_startup_data_mode, "Boot") &&
            !equals_ci(settings.imu_startup_data_mode, "GnssTimeKnown")) {
            throw ConfigError("receiver.imu.startup_data_mode must be Boot or GnssTimeKnown");
        }
        if (!equals_ci(settings.imu_orientation_mode, "SensorDefault") &&
            !equals_ci(settings.imu_orientation_mode, "manual") &&
            !equals_ci(settings.imu_orientation_mode, "fixed")) {
            throw ConfigError("receiver.imu.orientation_mode must be SensorDefault, manual, or fixed");
        }
        if (!settings.ant_lever_arm_configured) {
            throw ConfigError("receiver.imu.ant_lever_arm_m is required");
        }
        const auto in_lever_range = [](double v) { return v >= -100.0 && v <= 100.0; };
        if (!in_lever_range(settings.ant_lever_arm_m.x) ||
            !in_lever_range(settings.ant_lever_arm_m.y) ||
            !in_lever_range(settings.ant_lever_arm_m.z)) {
            throw ConfigError("receiver.imu.ant_lever_arm_m components must be in [-100, 100] meters");
        }
        if (!equals_ci(settings.gnss_attitude_mode, "none") &&
            !equals_ci(settings.gnss_attitude_mode, "MultiAntenna")) {
            throw ConfigError("receiver.gnss_attitude.mode must be none or MultiAntenna");
        }
        if (settings.cn0_mask_dbhz < 0 || settings.cn0_mask_dbhz > 60) {
            throw ConfigError("receiver.tracking.cn0_mask_dbhz must be in range 0..60 dB-Hz");
        }
        if (settings.streams.empty()) {
            throw ConfigError("receiver.streams must contain at least one stream");
        }
        for (const auto &s: settings.streams) {
            // Validate with a placeholder descriptor; the real one is only known
            // once connected.
            (void) build_sbf_output_command(s, "IP10");
        }
    }

    std::string redact_cmd(const std::string &cmd) {
        if (cmd.rfind("login", 0) == 0) {
            return "login, <REDACTED>, <REDACTED>";
        }
        return cmd;
    }
} // namespace asterx
