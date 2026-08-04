#include "app_config.hpp"

#include <cctype>
#include <set>
#include <yaml-cpp/yaml.h>

#include "string_util.h"
#include "utility.h"

namespace jai {
    namespace {
        std::string read_enum(const YAML::Node &n, const std::vector<std::string> &allowed, const std::string &path) {
            const auto s = n.as<std::string>();
            if (std::find(allowed.begin(), allowed.end(), s) != allowed.end()) {
                return s;
            }
            std::string msg = path + ": invalid value \"" + s + "\"; allowed:";
            for (const auto &a: allowed) {
                msg += " " + a;
            }
            throw ConfigError(msg);
        }


        std::vector<RawFeature> parse_raw_features(const YAML::Node &n, const std::string &path) {
            std::vector<RawFeature> out;
            for (const auto &item: n) {
                RawFeature f;
                if (item["name"]) {
                    f.name = item["name"].as<std::string>();
                }
                if (f.name.empty()) {
                    throw ConfigError(path + ": every entry requires a non-empty \"name\"");
                }
                const YAML::Node v = item["value"];
                if (!v || v.IsNull()) {
                    f.value = ""; // command features: value ignored
                } else {
                    // A QUOTED scalar means "apply as string/enum entry"; a plain
                    // scalar (true / 100 / 1.5) is applied by the camera node type
                    f.value = v.Scalar();
                    f.value_is_string = v.Tag() == "!";
                }
                out.push_back(std::move(f));
            }
            return out;
        }


        CameraConfig parse_camera(const YAML::Node &n, const std::string &path) {
            CameraConfig c;
            if (n["id"]) {
                c.id = n["id"].as<std::string>();
            }
            if (c.id.empty() || c.id.size() > 63) {
                throw ConfigError(path + ".id is required and must be 1..63 characters");
            }
            for (char ch: c.id) {
                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
                    throw ConfigError(path + ".id: only [A-Za-z0-9_-] allowed (used as directory name)");
                }
            }
            if (n["enabled"]) {
                c.enabled = n["enabled"].as<bool>();
            }

            if (auto d = n["device"]) {
                if (d["mac"]) {
                    c.device.mac = d["mac"].as<std::string>();
                }
                if (d["ip"]) {
                    c.device.ip = d["ip"].as<std::string>();
                }
                if (auto f = d["force_ip"]) {
                    if (f["enabled"]) {
                        c.device.force_ip.enabled = f["enabled"].as<bool>();
                    }
                    if (f["ip"]) {
                        c.device.force_ip.ip = f["ip"].as<std::string>();
                    }
                    if (f["subnet_mask"]) {
                        c.device.force_ip.subnet_mask = f["subnet_mask"].as<std::string>();
                    }
                    if (f["gateway"]) {
                        c.device.force_ip.gateway = f["gateway"].as<std::string>();
                    }
                }
            }
            if (c.device.mac.empty() && c.device.ip.empty()) {
                throw ConfigError(path + ".device requires mac or ip");
            }
            if (!c.device.mac.empty() && Common::StringUtil::NormalizeMac(c.device.mac).empty()) {
                throw ConfigError(path + ".device.mac is not a valid MAC address");
            }
            if (c.device.force_ip.enabled) {
                // FORCEIP addresses the camera by MAC, before it has a usable IP
                if (c.device.mac.empty()) {
                    throw ConfigError(path + ".device.force_ip requires device.mac");
                }
                if (c.device.force_ip.ip.empty() || c.device.force_ip.subnet_mask.empty()) {
                    throw ConfigError(path + ".device.force_ip.enabled requires ip and subnet_mask");
                }
            }

            if (auto a = n["acquisition"]) {
                if (a["exposure_ms"]) {
                    c.acquisition.exposure_ms = a["exposure_ms"].as<double>();
                }
                if (a["gain"]) {
                    c.acquisition.gain = a["gain"].as<double>();
                }
                if (a["frame_rate_hz"]) {
                    c.acquisition.frame_rate_hz = a["frame_rate_hz"].as<double>();
                }
                if (a["pixel_format"]) {
                    c.acquisition.pixel_format = a["pixel_format"].as<std::string>();
                }
                if (auto roi = a["roi"]) {
                    RoiConfig r;
                    if (roi["width"]) {
                        r.width = roi["width"].as<std::uint32_t>();
                    }
                    if (roi["height"]) {
                        r.height = roi["height"].as<std::uint32_t>();
                    }
                    if (roi["offset_x"]) {
                        r.offset_x = roi["offset_x"].as<std::uint32_t>();
                    }
                    if (roi["offset_y"]) {
                        r.offset_y = roi["offset_y"].as<std::uint32_t>();
                    }
                    c.acquisition.roi = r;
                }
                if (auto t = a["trigger"]) {
                    if (t["mode"]) {
                        c.acquisition.trigger.mode = read_enum(t["mode"], {"freerun", "external"},
                                                               path + ".acquisition.trigger.mode");
                    }
                    if (t["activation"]) {
                        c.acquisition.trigger.activation = read_enum(t["activation"], {"rising", "falling"},
                                                                     path + ".acquisition.trigger.activation");
                    }
                    if (t["selector_entry"]) {
                        c.acquisition.trigger.selector_entry = t["selector_entry"].as<std::string>();
                    }
                    if (t["source_entry"]) {
                        c.acquisition.trigger.source_entry = t["source_entry"].as<std::string>();
                    }
                }
            }

            if (auto f = n["features"]) {
                if (auto raw = f["raw"]; raw && raw.IsSequence()) {
                    c.features.raw = parse_raw_features(raw, path + ".features.raw");
                }
            }

            if (auto net = n["network"]) {
                if (net["channel"]) {
                    c.network.channel = net["channel"].as<std::uint32_t>();
                }
                if (net["buffer_count"]) {
                    c.network.buffer_count = net["buffer_count"].as<std::uint32_t>();
                    if (c.network.buffer_count != 0 && c.network.buffer_count < 4) {
                        throw ConfigError(path + ".network.buffer_count must be 0 (auto) or >= 4");
                    }
                }
                if (net["packet_size"]) {
                    c.network.packet_size = net["packet_size"].as<std::uint32_t>();
                    if (c.network.packet_size != 0 &&
                        (c.network.packet_size < 576 || c.network.packet_size > 16000)) {
                        throw ConfigError(path + ".network.packet_size must be 0 (auto) or in [576, 16000]");
                    }
                }
                if (net["socket_rx_buffer_mb"]) {
                    c.network.socket_rx_buffer_mb = net["socket_rx_buffer_mb"].as<std::uint32_t>();
                    if (c.network.socket_rx_buffer_mb == 0) {
                        throw ConfigError(path + ".network.socket_rx_buffer_mb must be > 0");
                    }
                }
                if (net["local_ip"]) {
                    c.network.local_ip = net["local_ip"].as<std::string>();
                }
                if (net["gev_scpd_ticks"]) {
                    c.network.gev_scpd_ticks = net["gev_scpd_ticks"].as<std::uint32_t>();
                }
                if (net["receiver_tuning"]) {
                    c.network.receiver_tuning =
                            parse_raw_features(net["receiver_tuning"], path + ".network.receiver_tuning");
                }
            }
            return c;
        }


        AppConfig parse_root(const YAML::Node &root) {
            AppConfig cfg;
            if (!root.IsDefined() || root.IsNull()) {
                throw ConfigError("Config error: empty document (cameras is required)");
            }

            try {
                if (auto n = root["output"]) {
                    if (n["output_dir"]) {
                        cfg.output.output_dir = n["output_dir"].as<std::string>();
                    }
                    if (n["segment_size_gib"]) {
                        cfg.output.segment_size_gib = n["segment_size_gib"].as<double>();
                        if (cfg.output.segment_size_gib <= 0) {
                            throw ConfigError("output.segment_size_gib must be > 0");
                        }
                    }
                    if (n["record_align"]) {
                        cfg.output.record_align = n["record_align"].as<std::uint32_t>();
                        if (cfg.output.record_align == 0 ||
                            (cfg.output.record_align & (cfg.output.record_align - 1)) != 0) {
                            throw ConfigError("output.record_align must be a power of two (1 disables alignment)");
                        }
                    }
                    if (n["queue_max_frames"]) {
                        cfg.output.queue_max_frames = n["queue_max_frames"].as<std::uint32_t>();
                        if (cfg.output.queue_max_frames < 2) {
                            throw ConfigError("output.queue_max_frames must be >= 2");
                        }
                    }
                    if (n["queue_on_full"]) {
                        cfg.output.queue_on_full =
                                read_enum(n["queue_on_full"], {"drop_newest", "block"}, "output.queue_on_full");
                    }
                    if (n["on_buffer_error"]) {
                        cfg.output.on_buffer_error =
                                read_enum(n["on_buffer_error"], {"record_flagged", "drop"}, "output.on_buffer_error");
                    }
                    if (n["flush_interval_mb"]) {
                        cfg.output.flush_interval_mb = n["flush_interval_mb"].as<std::uint32_t>();
                        if (cfg.output.flush_interval_mb == 0) {
                            throw ConfigError("output.flush_interval_mb must be > 0");
                        }
                    }
                    if (n["max_frames"]) {
                        cfg.output.max_frames = n["max_frames"].as<std::uint64_t>();
                    }
                    if (n["max_duration_s"]) {
                        cfg.output.max_duration_s = n["max_duration_s"].as<double>();
                    }
                }

                if (auto n = root["disk"]) {
                    if (n["min_free_gb"]) {
                        cfg.disk.min_free_gb = n["min_free_gb"].as<double>();
                    }
                }

                if (auto n = root["watchdog"]) {
                    if (n["no_frame_warn_s"]) {
                        cfg.watchdog.no_frame_warn_s = n["no_frame_warn_s"].as<double>();
                    }
                    if (n["no_frame_abort_s"]) {
                        cfg.watchdog.no_frame_abort_s = n["no_frame_abort_s"].as<double>();
                    }
                }

                if (auto n = root["ptp"]) {
                    if (n["enabled"]) {
                        cfg.ptp.enabled = n["enabled"].as<bool>();
                    }
                    if (n["feature_set"]) {
                        cfg.ptp.feature_set = read_enum(n["feature_set"],
                                                        {"auto", "gev_ieee1588", "sfnc_ptp", "explicit"},
                                                        "ptp.feature_set");
                    }
                    if (n["enable_feature"]) {
                        cfg.ptp.enable_feature = n["enable_feature"].as<std::string>();
                    }
                    if (n["status_feature"]) {
                        cfg.ptp.status_feature = n["status_feature"].as<std::string>();
                    }
                    if (n["required_status"]) {
                        cfg.ptp.required_status = n["required_status"].as<std::string>();
                    }
                    if (n["sync_timeout_s"]) {
                        cfg.ptp.sync_timeout_s = n["sync_timeout_s"].as<double>();
                    }
                    if (n["poll_interval_ms"]) {
                        cfg.ptp.poll_interval_ms = n["poll_interval_ms"].as<std::uint32_t>();
                        if (cfg.ptp.poll_interval_ms == 0) {
                            throw ConfigError("ptp.poll_interval_ms must be > 0");
                        }
                    }
                    if (n["on_timeout"]) {
                        cfg.ptp.on_timeout = read_enum(n["on_timeout"], {"abort", "warn_continue"}, "ptp.on_timeout");
                    }
                    if (n["offset_report_interval_s"]) {
                        cfg.ptp.offset_report_interval_s = n["offset_report_interval_s"].as<double>();
                    }
                    if (n["expect_tai_offset"]) {
                        cfg.ptp.expect_tai_offset = n["expect_tai_offset"].as<bool>();
                    }
                    if (cfg.ptp.feature_set == "explicit" &&
                        (cfg.ptp.enable_feature.empty() || cfg.ptp.status_feature.empty())) {
                        throw ConfigError("ptp: feature_set \"explicit\" requires enable_feature and status_feature");
                    }
                }

                if (auto n = root["logging"]) {
                    if (n["stats_interval_s"]) {
                        cfg.stats_interval_s = n["stats_interval_s"].as<double>();
                        if (cfg.stats_interval_s <= 0) {
                            throw ConfigError("logging.stats_interval_s must be > 0");
                        }
                    }
                }

                const YAML::Node cameras = root["cameras"];
                if (!cameras || !cameras.IsSequence() || cameras.size() == 0) {
                    throw ConfigError("cameras must be a non-empty list");
                }
                std::size_t idx = 0;
                for (const auto &cam: cameras) {
                    cfg.cameras.push_back(parse_camera(cam, "cameras[" + std::to_string(idx) + "]"));
                    ++idx;
                }
                std::set<std::string> ids;
                std::size_t enabled_count = 0;
                for (const auto &cam: cfg.cameras) {
                    if (!ids.insert(cam.id).second) {
                        throw ConfigError("Cameras: duplicate camera id \"" + cam.id + "\"");
                    }
                    if (cam.enabled) {
                        ++enabled_count;
                    }
                }
                if (enabled_count == 0) {
                    throw ConfigError("Cameras: at least one camera must be enabled");
                }
            } catch (const ConfigError &) {
                throw;
            } catch (const std::exception &e) {
                throw ConfigError(std::string("Invalid config: ") + e.what());
            }

            return cfg;
        }
    } // namespace


    AppConfig load_config(const std::string &path) {
        YAML::Node root;
        try {
            // Unified loading path (ConfigLoader logs and throws on failure)
            root = Common::ConfigLoader(path).root();
        } catch (const std::exception &e) {
            throw ConfigError(std::string("Failed to load config '") + path + "': " + e.what());
        }
        return parse_root(root);
    }

    AppConfig load_config_text(const std::string &yaml_text) {
        YAML::Node root;
        try {
            root = YAML::Load(yaml_text);
        } catch (const YAML::ParserException &e) {
            throw ConfigError(std::string("YAML parse error: ") + e.what());
        }
        return parse_root(root);
    }
} // namespace jai
