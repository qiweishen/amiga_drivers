#include "app_config.h"

#include <cctype>
#include <set>

#include "logger.h"
#include "string_util.h"


namespace gox {
    namespace {
        using namespace common::ConfigUtil;

        common::DriverLog g_log{"GoX"};

        // 0 = keep the device value (manual p.133-134)
        void CheckRoiAxis(uint32_t value, uint32_t min_value, uint32_t step, const std::string &path) {
            if (value == 0) {
                return;
            }
            if (value < min_value) {
                Fail(path, "must be 0 (keep) or >= " + std::to_string(min_value));
            }
            if (value % step != 0) {
                Fail(path, "must be a multiple of " + std::to_string(step));
            }
        }

        // TriggerSource: an integer from the GO-X's list (manual p.141) or a symbolic entry
        // name, verified against the camera at apply time.
        void CheckTriggerSource(const std::string &value, const std::string &path) {
            if (value.empty()) {
                Fail(path, "must not be empty");
            }
            if (value.find_first_not_of("0123456789") != std::string::npos) {
                return;
            }
            static const std::set<int> kAllowed = {
                7, 8, 9, 10, // PulseGenerator0-3
                11, 12, 13, 14, // UserOutput0-3
                15, 16, 17, 18, // Action0-3
                19, // Software
                24, // Line5 Opt In
                36, 37 // Nand0/1 Out
            };
            if (value.size() > 6 || kAllowed.count(std::stoi(value)) == 0) {
                Fail(path, "\"" + value + "\" is not a GO-X TriggerSource value; allowed: 7-19, 24, 36, 37 "
                           "(24 = Line5 Opt In, the hardware trigger input)");
            }
        }

        std::vector<RawFeature> ParseRawFeatures(const YAML::Node &list, const std::string &path) {
            std::vector<RawFeature> out;
            std::size_t i = 0;
            for (const auto &item: list) {
                const std::string item_path = IndexPath(path, i++);
                CheckKeys(item, item_path, {"name", "value"});
                RawFeature f;
                f.name = RequireText(item, item_path, "name");
                const YAML::Node v = Child(item, "value");
                if (v.IsDefined() && !v.IsNull()) {
                    if (!v.IsScalar()) {
                        Fail(item_path + ".value", "must be a scalar");
                    }
                    // Quoted = enum entry / string; plain (true / 100 / 1.5) = typed by the camera node
                    f.value = v.Scalar();
                    f.value_is_string = v.Tag() == "!";
                }
                out.push_back(std::move(f));
            }
            return out;
        }

        void ParseAcquisition(const YAML::Node &cam, const std::string &path, AcquisitionConfig &acq) {
            const YAML::Node a = OptionalMap(cam, path, "acquisition",
                                             {"exposure_ms", "gain", "frame_rate_hz", "pixel_format",
                                              "blemish_correction", "trigger", "roi"});
            const std::string p = path + ".acquisition";
            if (Read(a, p, "exposure_ms", acq.exposure_ms) && *acq.exposure_ms <= 0.0) {
                Fail(p + ".exposure_ms", "must be > 0");
            }
            if (Read(a, p, "gain", acq.gain) && (*acq.gain < 1.0 || *acq.gain > 126.0)) {
                Fail(p + ".gain", "must be in [1.0, 126.0] (magnification, not dB)");
            }
            // The longest frame period is 8 s (manual p.36/p.141)
            if (Read(a, p, "frame_rate_hz", acq.frame_rate_hz) && *acq.frame_rate_hz < 0.125) {
                Fail(p + ".frame_rate_hz", "must be >= 0.125");
            }
            Read(a, p, "pixel_format", acq.pixel_format);
            Read(a, p, "blemish_correction", acq.blemish_correction);

            const YAML::Node roi = OptionalMap(a, p, "roi", {"width", "height", "offset_x", "offset_y"});
            if (roi.IsDefined()) {
                RoiConfig r;
                Read(roi, p + ".roi", "width", r.width);
                Read(roi, p + ".roi", "height", r.height);
                Read(roi, p + ".roi", "offset_x", r.offset_x);
                Read(roi, p + ".roi", "offset_y", r.offset_y);
                CheckRoiAxis(r.width, 96, 8, p + ".roi.width");
                CheckRoiAxis(r.height, 8, 2, p + ".roi.height");
                if (r.offset_x % 8 != 0) {
                    Fail(p + ".roi.offset_x", "must be a multiple of 8");
                }
                if (r.offset_y % 2 != 0) {
                    Fail(p + ".roi.offset_y", "must be a multiple of 2");
                }
                acq.roi = r;
            }

            const YAML::Node t = OptionalMap(a, p, "trigger", {"mode", "activation", "selector_entry", "source_entry"});
            const std::string tp = p + ".trigger";
            ReadEnum<TriggerMode>(t, tp, "mode", {{"freerun", TriggerMode::kFreerun}, {"external", TriggerMode::kExternal}},
                                  acq.trigger.mode);
            ReadEnum<TriggerActivation>(t, tp, "activation",
                                        {{"rising", TriggerActivation::kRising}, {"falling", TriggerActivation::kFalling}},
                                        acq.trigger.activation);
            ReadText(t, tp, "selector_entry", acq.trigger.selector_entry);
            if (Read(t, tp, "source_entry", acq.trigger.source_entry)) {
                CheckTriggerSource(acq.trigger.source_entry, tp + ".source_entry");
            }
        }

        void ParseNetwork(const YAML::Node &cam, const std::string &path, NetworkConfig &net) {
            const YAML::Node n = OptionalMap(cam, path, "network",
                                             {"buffer_count", "packet_size", "socket_rx_buffer_mb", "local_ip",
                                              "throughput_safety_margin_pct", "receiver_tuning"});
            const std::string p = path + ".network";
            if (Read(n, p, "buffer_count", net.buffer_count) && net.buffer_count != 0 && net.buffer_count < 4) {
                Fail(p + ".buffer_count", "must be 0 (auto) or >= 4");
            }
            // GevSCPSPacketSize range (manual p.130)
            if (Read(n, p, "packet_size", net.packet_size) && net.packet_size != 0 &&
                (net.packet_size < 1476 || net.packet_size > 12036)) {
                Fail(p + ".packet_size", "must be 0 (auto) or in [1476, 12036]");
            }
            ReadRange<uint32_t>(n, p, "socket_rx_buffer_mb", 1, 1024, net.socket_rx_buffer_mb);
            Read(n, p, "local_ip", net.local_ip);
            if (Read(n, p, "throughput_safety_margin_pct", net.throughput_safety_margin_pct) &&
                net.throughput_safety_margin_pct != 0 &&
                (net.throughput_safety_margin_pct < 10 || net.throughput_safety_margin_pct > 100)) {
                Fail(p + ".throughput_safety_margin_pct", "must be 0 (factory 92) or in [10, 100]");
            }
            const YAML::Node rt = OptionalSequence(n, p, "receiver_tuning");
            if (rt.IsDefined()) {
                net.receiver_tuning = ParseRawFeatures(rt, p + ".receiver_tuning");
            }
        }

        CameraConfig ParseCamera(const YAML::Node &n, const std::string &path) {
            CheckKeys(n, path, {"id", "enabled", "device", "acquisition", "features", "network"});
            CameraConfig c;
            c.id = RequireText(n, path, "id");
            if (c.id.size() > 63) {
                Fail(path + ".id", "must be 1..63 characters");
            }
            for (const char ch: c.id) {
                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
                    Fail(path + ".id", "only [A-Za-z0-9_-] allowed (used as directory name)");
                }
            }
            Read(n, path, "enabled", c.enabled);

            const YAML::Node d = OptionalMap(n, path, "device", {"mac", "ip"});
            Read(d, path + ".device", "mac", c.device.mac);
            Read(d, path + ".device", "ip", c.device.ip);
            if (c.device.mac.empty() && c.device.ip.empty()) {
                Fail(path + ".device", "requires mac or ip");
            }
            if (!c.device.mac.empty() && common::StringUtil::NormalizeMac(c.device.mac).empty()) {
                Fail(path + ".device.mac", "is not a valid MAC address");
            }

            ParseAcquisition(n, path, c.acquisition);

            const YAML::Node f = OptionalMap(n, path, "features", {"raw"});
            const YAML::Node raw = OptionalSequence(f, path + ".features", "raw");
            if (raw.IsDefined()) {
                c.features.raw = ParseRawFeatures(raw, path + ".features.raw");
            }

            ParseNetwork(n, path, c.network);

            const auto &acq = c.acquisition;
            if (acq.exposure_ms && acq.frame_rate_hz) {
                const double period_ms = 900.0 / *acq.frame_rate_hz;
                if (*acq.exposure_ms >= period_ms) {
                    Fail(path + ".acquisition", "exposure_ms (" + std::to_string(*acq.exposure_ms) +
                                                ") must be below the frame period 900/frame_rate_hz (" +
                                                std::to_string(period_ms) + " ms); lower the exposure or the frame rate");
                }
            }
            g_log.Trace("Loaded camera instance [{}] ({})", c.id, c.device.mac.empty() ? c.device.ip : c.device.mac);
            return c;
        }

        AppConfig ParseRoot(const YAML::Node &root) {
            CheckKeys(root, "", {"cameras", "output", "ptp", "logging"});
            AppConfig cfg;

            const YAML::Node out = OptionalMap(root, "", "output",
                                               {"segment_size_mb", "record_align", "queue_max_frames", "queue_on_full",
                                                "on_buffer_error", "flush_interval_mb", "max_frames", "max_duration_s"});
            ReadRange<uint32_t>(out, "output", "segment_size_mb", 1, 1024 * 1024, cfg.output.segment_size_mb);
            if (Read(out, "output", "record_align", cfg.output.record_align) &&
                (cfg.output.record_align == 0 || (cfg.output.record_align & (cfg.output.record_align - 1)) != 0)) {
                Fail("output.record_align", "must be a power of two (1 disables alignment)");
            }
            // 0 = auto (buffering.hpp); an explicit value leaves one chunk for the writer and one for the acquirer
            if (Read(out, "output", "queue_max_frames", cfg.output.queue_max_frames) && cfg.output.queue_max_frames == 1) {
                Fail("output.queue_max_frames", "must be >= 2 (or 0 for auto)");
            }
            ReadEnum<QueueOnFull>(out, "output", "queue_on_full",
                                  {{"drop_newest", QueueOnFull::kDropNewest}, {"block", QueueOnFull::kBlock}},
                                  cfg.output.queue_on_full);
            ReadEnum<OnBufferError>(out, "output", "on_buffer_error",
                                    {{"record_flagged", OnBufferError::kRecordFlagged}, {"drop", OnBufferError::kDrop}},
                                    cfg.output.on_buffer_error);
            ReadRange<uint32_t>(out, "output", "flush_interval_mb", 1, 1024 * 1024, cfg.output.flush_interval_mb);
            Read(out, "output", "max_frames", cfg.output.max_frames);
            ReadRange<double>(out, "output", "max_duration_s", 0.0, 86400.0, cfg.output.max_duration_s);

            const YAML::Node ptp = OptionalMap(root, "", "ptp", {"enabled", "sync_timeout_s", "on_timeout"});
            Read(ptp, "ptp", "enabled", cfg.ptp.enabled);
            if (Read(ptp, "ptp", "sync_timeout_s", cfg.ptp.sync_timeout_s) && cfg.ptp.sync_timeout_s <= 0) {
                Fail("ptp.sync_timeout_s", "must be > 0");
            }
            ReadEnum<PtpOnTimeout>(ptp, "ptp", "on_timeout",
                                   {{"abort", PtpOnTimeout::kAbort}, {"warn_continue", PtpOnTimeout::kWarnContinue}},
                                   cfg.ptp.on_timeout);

            const YAML::Node logging = OptionalMap(root, "", "logging", {"stats_interval_s"});
            if (Read(logging, "logging", "stats_interval_s", cfg.stats_interval_s) && cfg.stats_interval_s <= 0) {
                Fail("logging.stats_interval_s", "must be > 0");
            }

            const YAML::Node cameras = RequireSequence(root, "", "cameras");
            if (cameras.size() == 0) {
                Fail("cameras", "must be a non-empty list");
            }
            std::size_t idx = 0;
            for (const auto &cam: cameras) {
                cfg.cameras.push_back(ParseCamera(cam, IndexPath("cameras", idx++)));
            }
            std::set<std::string> ids;
            std::size_t enabled_count = 0;
            for (const auto &cam: cfg.cameras) {
                if (!ids.insert(cam.id).second) {
                    Fail("cameras", "duplicate camera id \"" + cam.id + "\"");
                }
                enabled_count += cam.enabled ? 1 : 0;
            }
            if (enabled_count == 0) {
                Fail("cameras", "at least one camera must be enabled");
            }
            g_log.Trace("Loaded {} cam instance(s)", enabled_count);
            return cfg;
        }
    } // namespace


    AppConfig LoadAppConfig(const std::string &path) {
        return ParseRoot(LoadFile(path));
    }


    AppConfig LoadAppConfigText(const std::string &yaml_text) {
        return ParseRoot(LoadText(yaml_text));
    }
} // namespace gox
