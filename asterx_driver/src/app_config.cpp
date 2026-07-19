// SPDX-License-Identifier: BSD-3-Clause
#include "app_config.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace asterx {
    namespace {
        std::string lower_copy(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        std::uint16_t read_port(const YAML::Node &n, const char *key) {
            const int value = n[key].as<int>();
            if (value < 1 || value > 65535) {
                throw ConfigLoadError(std::string(key) + " must be in range 1..65535");
            }
            return static_cast<std::uint16_t>(value);
        }

        Vec3 read_vec3(const YAML::Node &n, const std::string &path) {
            if (!n) {
                throw ConfigLoadError(path + " is required");
            }
            if (n.IsSequence()) {
                if (n.size() != 3) {
                    throw ConfigLoadError(path + " sequence must contain exactly three values");
                }
                return Vec3{n[0].as<double>(), n[1].as<double>(), n[2].as<double>()};
            }
            if (!n.IsMap() || !n["x"] || !n["y"] || !n["z"]) {
                throw ConfigLoadError(path + " must contain x, y, and z");
            }
            return Vec3{n["x"].as<double>(), n["y"].as<double>(), n["z"].as<double>()};
        }

        AttitudeOffset read_attitude_offset(const YAML::Node &n) {
            if (!n) {
                return AttitudeOffset{};
            }
            if (!n.IsMap() || !n["heading"] || !n["pitch"]) {
                throw ConfigLoadError("receiver.gnss_attitude.attitude_offset_deg must contain heading and pitch");
            }
            return AttitudeOffset{n["heading"].as<double>(), n["pitch"].as<double>()};
        }

        void parse_receiver(const YAML::Node &n, ReceiverSettings &receiver) {
            if (n["ips_id"] || n["ips_port"]) {
                throw ConfigLoadError(
                    "receiver.ips_id / receiver.ips_port were removed: the driver now "
                    "receives SBF on the command connection itself (single-connection "
                    "design) — delete these fields from the config");
            }
            if (n["require_aux1"]) {
                receiver.require_aux1 = n["require_aux1"].as<bool>();
            }

            if (auto im = n["imu"]) {
                if (im["startup_data_mode"]) {
                    receiver.imu_startup_data_mode = im["startup_data_mode"].as<std::string>();
                }

                if (im["orientation_mode"]) {
                    receiver.imu_orientation_mode = im["orientation_mode"].as<std::string>();
                } else if (im["use_sensor_default"]) {
                    receiver.use_sensor_default = im["use_sensor_default"].as<bool>();
                    receiver.imu_orientation_mode = receiver.use_sensor_default ? "SensorDefault" : "manual";
                }

                if (im["theta_x_deg"]) {
                    receiver.theta_x_deg = im["theta_x_deg"].as<double>();
                }
                if (im["theta_y_deg"]) {
                    receiver.theta_y_deg = im["theta_y_deg"].as<double>();
                }
                if (im["theta_z_deg"]) {
                    receiver.theta_z_deg = im["theta_z_deg"].as<double>();
                }
                if (im["ant_lever_arm_m"]) {
                    receiver.ant_lever_arm_m = read_vec3(im["ant_lever_arm_m"],
                                                         "receiver.imu.ant_lever_arm_m");
                    receiver.ant_lever_arm_configured = true;
                }
            }

            if (auto att = n["gnss_attitude"]) {
                if (att["mode"]) {
                    receiver.gnss_attitude_mode = att["mode"].as<std::string>();
                }
                if (att["attitude_offset_deg"]) {
                    receiver.attitude_offset_deg =
                            read_attitude_offset(att["attitude_offset_deg"]);
                }
            }

            if (auto tr = n["tracking"]) {
                if (tr["enable_all"]) {
                    receiver.configure_all_tracking = tr["enable_all"].as<bool>();
                }
                if (tr["cn0_mask_dbhz"]) {
                    receiver.cn0_mask_dbhz = tr["cn0_mask_dbhz"].as<int>();
                }
            }

            if (n["streams"] && n["streams"].IsSequence()) {
                receiver.streams.clear();
                for (const auto &s: n["streams"]) {
                    SbfStream st;
                    st.stream_id = s["id"].as<int>();
                    st.interval = s["interval"].as<std::string>();
                    for (const auto &b: s["blocks"]) {
                        st.blocks.push_back(b.as<std::string>());
                    }
                    receiver.streams.push_back(std::move(st));
                }
            }

            if (lower_copy(receiver.imu_orientation_mode) == "sensordefault") {
                receiver.use_sensor_default = true;
            } else {
                receiver.use_sensor_default = false;
            }
        }
    } // namespace

    AppConfig load_app_config(const std::string &path) {
        AppConfig c;
        YAML::Node root;
        try {
            root = YAML::LoadFile(path);
        } catch (const std::exception &e) {
            throw ConfigLoadError(std::string("failed to load config '") + path + "': " + e.what());
        }

        try {
            if (auto n = root["connection"]) {
                if (n["host"]) {
                    c.host = n["host"].as<std::string>();
                }
                if (n["port"]) {
                    c.ctrl_port = read_port(n, "port");
                }
                if (n["user"]) {
                    c.user = n["user"].as<std::string>();
                }
                if (n["password"]) {
                    c.password = n["password"].as<std::string>();
                }
            }
            if (auto n = root["output"]) {
                if (n["dir"]) {
                    c.output_dir = n["dir"].as<std::string>();
                }
                if (n["file_prefix"]) {
                    c.file_prefix = n["file_prefix"].as<std::string>();
                }
                if (n["rotate_bytes"]) {
                    c.rotate_bytes = n["rotate_bytes"].as<std::uint64_t>();
                }
                if (n["rotate_interval_s"]) {
                    c.rotate_interval_seconds = n["rotate_interval_s"].as<int>();
                }
            }
            if (auto n = root["log"]) {
                if (n["level"]) {
                    c.log_level = n["level"].as<std::string>();
                }
            }
            if (auto n = root["receiver"]) {
                parse_receiver(n, c.receiver);
            }

            // sbf2rin refuses SBF inputs of 2 GB or larger; rotation doubles as
            // the post-processing guarantee.
            if (c.rotate_bytes >= (2ull << 30)) {
                throw ConfigLoadError(
                    "output.rotate_bytes must be < 2 GiB (sbf2rin cannot convert "
                    "SBF files of 2 GB or larger)");
            }
            if (c.rotate_interval_seconds <= 0) {
                throw ConfigLoadError("output.rotate_interval_s must be positive");
            }

            c.receiver.user = c.user;
            c.receiver.password = c.password;
            validate_receiver_settings(c.receiver);
        } catch (const ConfigLoadError &) {
            throw;
        } catch (const ConfigError &e) {
            throw ConfigLoadError(e.what());
        } catch (const std::exception &e) {
            throw ConfigLoadError(std::string("invalid config '") + path + "': " + e.what());
        }

        return c;
    }
} // namespace asterx
