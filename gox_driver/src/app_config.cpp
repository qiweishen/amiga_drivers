#include "app_config.hpp"

#include <cctype>
#include <set>
#include <sstream>
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

        AcquisitionLimits parse_acquisition(const YAML::Node &n, AcquisitionLimits c) {
            if (n["max_frames"]) {
                c.max_frames = n["max_frames"].as<std::uint64_t>();
            }
            if (n["max_duration_s"]) {
                c.max_duration_s = n["max_duration_s"].as<double>();
            }
            return c;
        }

        // Per-camera blocks start from the parsed GLOBAL struct as `c`, so a
        // camera override only needs the keys it changes (the struct-level
        // equivalent of the old JSON deep-merge; ptp/recording are flat).
        PtpConfig parse_ptp(const YAML::Node &n, PtpConfig c, const std::string &path) {
            if (n["enabled"]) {
                c.enabled = n["enabled"].as<bool>();
            }
            if (n["feature_set"]) {
                c.feature_set = read_enum(n["feature_set"], {"auto", "gev_ieee1588", "sfnc_ptp", "explicit"},
                                          path + ".feature_set");
            }
            if (n["enable_feature"]) {
                c.enable_feature = n["enable_feature"].as<std::string>();
            }
            if (n["status_feature"]) {
                c.status_feature = n["status_feature"].as<std::string>();
            }
            if (n["required_status"]) {
                c.required_status = n["required_status"].as<std::string>();
            }
            if (n["sync_timeout_s"]) {
                c.sync_timeout_s = n["sync_timeout_s"].as<double>();
            }
            if (n["poll_interval_ms"]) {
                c.poll_interval_ms = n["poll_interval_ms"].as<std::uint32_t>();
                if (c.poll_interval_ms == 0) {
                    throw ConfigError(path + ".poll_interval_ms must be > 0");
                }
            }
            if (n["on_timeout"]) {
                c.on_timeout = read_enum(n["on_timeout"], {"abort", "warn_continue"}, path + ".on_timeout");
            }
            if (n["offset_report_interval_s"]) {
                c.offset_report_interval_s = n["offset_report_interval_s"].as<double>();
            }
            if (n["expect_tai_offset"]) {
                c.expect_tai_offset = n["expect_tai_offset"].as<bool>();
            }
            if (c.feature_set == "explicit" && (c.enable_feature.empty() || c.status_feature.empty())) {
                throw ConfigError(path + ": feature_set \"explicit\" requires enable_feature and status_feature");
            }
            return c;
        }


        RecordingConfig parse_recording(const YAML::Node &n, RecordingConfig c, const std::string &path) {
            if (n["output_dir"]) {
                c.output_dir = n["output_dir"].as<std::string>();
            }
            if (n["session_name"]) {
                c.session_name = n["session_name"].as<std::string>();
            }
            if (n["segment_size_gib"]) {
                c.segment_size_gib = n["segment_size_gib"].as<double>();
                if (c.segment_size_gib <= 0) {
                    throw ConfigError(path + ".segment_size_gib must be > 0");
                }
            }
            if (n["record_align"]) {
                c.record_align = n["record_align"].as<std::uint32_t>();
                if (c.record_align == 0 || (c.record_align & (c.record_align - 1)) != 0) {
                    throw ConfigError(path + ".record_align must be a power of two (1 disables alignment)");
                }
            }
            if (n["queue_max_frames"]) {
                c.queue_max_frames = n["queue_max_frames"].as<std::uint32_t>();
                if (c.queue_max_frames < 2) {
                    throw ConfigError(path + ".queue_max_frames must be >= 2");
                }
            }
            if (n["queue_on_full"]) {
                c.queue_on_full = read_enum(n["queue_on_full"], {"drop_newest", "block"}, path + ".queue_on_full");
            }
            if (n["on_buffer_error"]) {
                c.on_buffer_error =
                        read_enum(n["on_buffer_error"], {"record_flagged", "drop"}, path + ".on_buffer_error");
            }
            if (n["flush_interval_mb"]) {
                c.flush_interval_mb = n["flush_interval_mb"].as<std::uint32_t>();
                if (c.flush_interval_mb == 0) {
                    throw ConfigError(path + ".flush_interval_mb must be > 0");
                }
            }
            if (n["min_free_gib"]) {
                c.min_free_gib = n["min_free_gib"].as<double>();
            }
            return c;
        }


        std::vector<GenicamFeature> parse_feature_list(const YAML::Node &n, const std::string &path) {
            std::vector<GenicamFeature> out;
            for (const auto &item: n) {
                GenicamFeature f;
                if (item["feature"]) {
                    f.feature = item["feature"].as<std::string>();
                }
                if (f.feature.empty()) {
                    throw ConfigError(path + ": every entry requires a non-empty \"feature\"");
                }
                const YAML::Node v = item["value"];
                if (!v || v.IsNull()) {
                    f.value = ""; // command features: value ignored
                } else {
                    // A QUOTED scalar means "apply as string/enum entry"; a plain
                    // scalar (true / 100 / 1.5) keeps the JSON-era typed semantics
                    f.value = v.Scalar();
                    f.value_is_string = v.Tag() == "!";
                }
                if (item["on_error"]) {
                    f.on_error = read_enum(item["on_error"], {"fail", "warn", "skip"}, path + ".on_error");
                }
                out.push_back(std::move(f));
            }
            return out;
        }


        ApplyConfig parse_apply(const YAML::Node &n) {
            ApplyConfig c;
            if (n["verify_readback"]) {
                c.verify_readback = n["verify_readback"].as<bool>();
            }
            if (n["float_verify_tolerance_rel"]) {
                c.float_verify_tolerance_rel = n["float_verify_tolerance_rel"].as<double>();
            }
            if (n["on_error_default"]) {
                c.on_error_default = read_enum(n["on_error_default"], {"fail", "warn", "skip"},
                                               "apply.on_error_default");
            }
            return c;
        }


        StreamConfig parse_stream(const YAML::Node &n, const std::string &path) {
            StreamConfig c;
            if (n["channel"]) {
                c.channel = n["channel"].as<std::uint32_t>();
            }
            if (n["buffer_count"]) {
                c.buffer_count = n["buffer_count"].as<std::uint32_t>();
                if (c.buffer_count != 0 && c.buffer_count < 4) {
                    throw ConfigError(path + ".buffer_count must be 0 (auto) or >= 4");
                }
            }
            if (n["packet_size"]) {
                c.packet_size = n["packet_size"].as<std::uint32_t>();
                if (c.packet_size != 0 && (c.packet_size < 576 || c.packet_size > 16000)) {
                    throw ConfigError(path + ".packet_size must be 0 (auto) or in [576, 16000]");
                }
            }
            if (n["socket_rx_buffer_mib"]) {
                c.socket_rx_buffer_mib = n["socket_rx_buffer_mib"].as<std::uint32_t>();
                if (c.socket_rx_buffer_mib == 0) {
                    throw ConfigError(path + ".socket_rx_buffer_mib must be > 0");
                }
            }
            if (n["local_ip"]) {
                c.local_ip = n["local_ip"].as<std::string>();
            }
            if (n["gev_scpd_ticks"]) {
                c.gev_scpd_ticks = n["gev_scpd_ticks"].as<std::uint32_t>();
            }
            if (n["receiver_tuning"]) {
                c.receiver_tuning = parse_feature_list(n["receiver_tuning"], path + ".receiver_tuning");
            }
            return c;
        }


        CameraConfig parse_camera(const YAML::Node &n, const PtpConfig &global_ptp,
                                  const RecordingConfig &global_recording, const std::string &path) {
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
            const YAML::Node sel = n["selector"];
            if (!sel || !sel["by"] || !sel["value"]) {
                throw ConfigError(path + R"(.selector requires "by" and "value")");
            }
            c.selector.by = read_enum(sel["by"], {"mac", "ip", "serial", "user_defined_name"},
                                      path + ".selector.by");
            c.selector.value = sel["value"].as<std::string>();
            if (c.selector.value.empty()) {
                throw ConfigError(path + ".selector.value must not be empty");
            }
            if (c.selector.by == "mac" && Common::StringUtil::NormalizeMac(c.selector.value).empty()) {
                throw ConfigError(path + ".selector.value is not a valid MAC address");
            }
            if (auto d = n["discovery"]) {
                if (d["timeout_ms"]) {
                    c.discovery.timeout_ms = d["timeout_ms"].as<std::uint32_t>();
                }
                if (d["retries"]) {
                    c.discovery.retries = d["retries"].as<std::uint32_t>();
                }
                if (d["retry_interval_ms"]) {
                    c.discovery.retry_interval_ms = d["retry_interval_ms"].as<std::uint32_t>();
                }
                if (auto fp = d["force_ip"]) {
                    if (fp["enabled"]) {
                        c.discovery.force_ip.enabled = fp["enabled"].as<bool>();
                    }
                    if (fp["ip"]) {
                        c.discovery.force_ip.ip = fp["ip"].as<std::string>();
                    }
                    if (fp["subnet_mask"]) {
                        c.discovery.force_ip.subnet_mask = fp["subnet_mask"].as<std::string>();
                    }
                    if (fp["gateway"]) {
                        c.discovery.force_ip.gateway = fp["gateway"].as<std::string>();
                    }
                    if (c.discovery.force_ip.enabled &&
                        (c.discovery.force_ip.ip.empty() || c.discovery.force_ip.subnet_mask.empty())) {
                        throw ConfigError(path + ".discovery.force_ip.enabled requires ip and subnet_mask");
                    }
                }
            }
            if (auto conv = n["convenience"]) {
                if (conv["exposure_us"]) {
                    c.convenience.exposure_us = conv["exposure_us"].as<double>();
                }
                if (conv["gain"]) {
                    c.convenience.gain = conv["gain"].as<double>();
                }
                if (conv["frame_rate"]) {
                    c.convenience.frame_rate = conv["frame_rate"].as<double>();
                }
                if (conv["pixel_format"]) {
                    c.convenience.pixel_format = conv["pixel_format"].as<std::string>();
                }
                if (auto roi = conv["roi"]) {
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
                    c.convenience.roi = r;
                }
                if (auto tr = conv["trigger"]) {
                    TriggerConfig t;
                    if (tr["enabled"]) {
                        t.enabled = tr["enabled"].as<bool>();
                    }
                    if (tr["selector"]) {
                        t.selector = tr["selector"].as<std::string>();
                    }
                    if (tr["source"]) {
                        t.source = tr["source"].as<std::string>();
                    }
                    if (tr["activation"]) {
                        t.activation = tr["activation"].as<std::string>();
                    }
                    c.convenience.trigger = t;
                }
            }
            if (n["genicam_features"]) {
                c.genicam_features = parse_feature_list(n["genicam_features"], path + ".genicam_features");
            }
            if (n["apply"]) {
                c.apply = parse_apply(n["apply"]);
            }
            if (n["stream"]) {
                c.stream = parse_stream(n["stream"], path + ".stream");
            }
            // Per-camera ptp/recording override the parsed globals key-by-key
            c.ptp = n["ptp"] ? parse_ptp(n["ptp"], global_ptp, path + ".ptp") : global_ptp;
            c.recording = n["recording"]
                              ? parse_recording(n["recording"], global_recording, path + ".recording")
                              : global_recording;
            return c;
        }


        AppConfig parse_root(const YAML::Node &root) {
            AppConfig cfg;
            if (!root.IsDefined() || root.IsNull()) {
                throw ConfigError("Config error: empty document (cameras is required)");
            }

            try {
                // GLOBAL blocks MUST be parsed before any camera: each camera's
                // ptp/recording copy is merged from the parsed globals, so a
                // camera parsed first would snapshot the struct defaults and
                // silently ignore the YAML's global values.
                if (auto n = root["ptp"]) {
                    cfg.ptp = parse_ptp(n, cfg.ptp, "ptp");
                }
                if (auto n = root["acquisition"]) {
                    cfg.acquisition = parse_acquisition(n, cfg.acquisition);
                }
                if (auto n = root["recording"]) {
                    cfg.recording = parse_recording(n, cfg.recording, "recording");
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
                    cfg.cameras.push_back(
                        parse_camera(cam, cfg.ptp, cfg.recording, "cameras[" + std::to_string(idx) + "]"));
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
