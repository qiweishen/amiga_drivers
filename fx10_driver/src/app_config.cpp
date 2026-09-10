#include "app_config.h"
#include "trigger_groups.h"

#include <cmath>
#include <sstream>

#include "string_util.h"


namespace fx10 {
    namespace {
        using namespace common::ConfigUtil;

        void ParseAcquisition(const YAML::Node &root, AcquisitionConfig &acq) {
            const YAML::Node n = OptionalMap(root, "", "acquisition",
                                             {
                                                 "spatial_binning", "spectral_binning", "pixel_format", "exposure_ms",
                                                 "frame_rate_hz", "status_line", "image_enhancement", "trigger", "mroi"
                                             });
            const std::string p = "acquisition";
            Read(n, p, "spatial_binning", acq.spatial_binning);
            Read(n, p, "spectral_binning", acq.spectral_binning);
            ReadEnum(n, p, "pixel_format", {"Mono12Packed", "Mono12", "Mono10Packed", "Mono10", "Mono8"},
                     acq.pixel_format);
            if (Read(n, p, "exposure_ms", acq.exposure_ms) && !(acq.exposure_ms > 0.0)) {
                Fail(p + ".exposure_ms", "must be > 0");
            }
            if (Read(n, p, "frame_rate_hz", acq.frame_rate_hz) && !(acq.frame_rate_hz > 0.0)) {
                Fail(p + ".frame_rate_hz", "must be > 0");
            }
            const double period_ms = 900.0 / acq.frame_rate_hz;
            if (acq.exposure_ms >= period_ms) {
                Fail(p + ".exposure_ms/.frame_rate_hz", "exposure_ms (" + std::to_string(acq.exposure_ms) +
                                            ") must be below the frame period 900/frame_rate_hz (" +
                                            std::to_string(period_ms) + " ms); lower the exposure or the frame rate");
            }
            Read(n, p, "status_line", acq.status_line);
            if (Read(n, p, "image_enhancement", acq.image_enhancement) && !acq.image_enhancement) {
                // The manual never prints the GenICam node for AIE (only the Lumo name, p.39)
                Fail(p + ".image_enhancement", "only true is supported (the AIE node name is unknown; "
                     "use features.raw to switch it off)");
            }

            const YAML::Node t = OptionalMap(n, p, "trigger",
                                             {
                                                 "mode", "activation", "delay_ms", "exposure_control", "selector_entry",
                                                 "source_entry"
                                             });
            const std::string tp = p + ".trigger";
            ReadEnum<TriggerMode>(t, tp, "mode",
                                  {{"freerun", TriggerMode::kFreerun}, {"external", TriggerMode::kExternal}},
                                  acq.trigger.mode);
            ReadEnum<TriggerActivation>(t, tp, "activation",
                                        {
                                            {"rising", TriggerActivation::kRising},
                                            {"falling", TriggerActivation::kFalling}
                                        },
                                        acq.trigger.activation);
            ReadRange<double>(t, tp, "delay_ms", 0.0, 419.0, acq.trigger.delay_ms);
            ReadEnum<ExposureControl>(t, tp, "exposure_control",
                                      {
                                          {"camera", ExposureControl::kCamera},
                                          {"pulse_width", ExposureControl::kPulseWidth}
                                      },
                                      acq.trigger.exposure_control);
            ReadText(t, tp, "selector_entry", acq.trigger.selector_entry);
            ReadText(t, tp, "source_entry", acq.trigger.source_entry);

            const YAML::Node m = OptionalMap(n, p, "mroi", {"enabled", "multiband_string"});
            Read(m, p + ".mroi", "enabled", acq.mroi.enabled);
            Read(m, p + ".mroi", "multiband_string", acq.mroi.multiband_string);

            const auto is_binning = [](int v) { return v == 1 || v == 2 || v == 4 || v == 8; };
            if (!is_binning(acq.spectral_binning)) {
                Fail(p + ".spectral_binning", "must be 1, 2, 4, or 8");
            }
            if (!is_binning(acq.spatial_binning)) {
                Fail(p + ".spatial_binning", "must be 1, 2, 4, or 8");
            }
            if (acq.mroi.enabled) {
                // Manual p.9, p.26: MROI requires 1 x 1 binning on BOTH axes
                if (acq.spectral_binning != 1 || acq.spatial_binning != 1) {
                    Fail(p + ".mroi", "requires 1 x 1 binning: spatial_binning and spectral_binning must both be 1 "
                         "(manual p.9, p.26)");
                }
                ParseMroiRegions(acq.mroi.multiband_string);
            }
        }

        void ParseSensorTrigger(const YAML::Node &root, SensorTriggerConfig &st, const AcquisitionConfig &acq) {
            const YAML::Node n = OptionalMap(root, "", "sensor_trigger", {"enabled", "port", "trigger_channel"});
            Read(n, "sensor_trigger", "enabled", st.enabled);
            Read(n, "sensor_trigger", "port", st.port);
            ReadRange<int>(n, "sensor_trigger", "trigger_channel", 0, 3, st.trigger_channel);
            if (st.enabled && st.port.empty()) {
                Fail("sensor_trigger.port", "is required when sensor_trigger.enabled "
                     "(a /dev/serial/by-id/... path survives USB re-enumeration)");
            }
            // Protocol v2 drives paired outputs: 0/1 = FX, 2/3 = JAI.
            const auto group = static_cast<trigger::Group>(st.trigger_channel / 2);
            if (st.enabled && acq.trigger.mode == TriggerMode::kExternal &&
                !trigger::validRate(group, acq.frame_rate_hz)) {
                Fail("acquisition.frame_rate_hz", group == trigger::Group::FX
                    ? "must be >= 20 Hz for the SensorSync FX group (outputs 0/1)"
                    : "must be within [1, 10] Hz for the SensorSync JAI group (outputs 2/3)");
            }
        }

        void ParseFeatures(const YAML::Node &root, FeatureConfig &features) {
            const YAML::Node n = OptionalMap(root, "", "features", {"raw"});
            const YAML::Node raw = OptionalSequence(n, "features", "raw");
            if (!raw.IsDefined()) {
                return;
            }
            std::size_t i = 0;
            for (const auto &item: raw) {
                const std::string p = IndexPath("features.raw", i++);
                CheckKeys(item, p, {"name", "type", "value"});
                RawFeature f;
                f.name = RequireText(item, p, "name");
                if (f.name == "AcquisitionStart" || f.name == "AcquisitionStop" ||
                    f.name == "MotorShutter_PulseRev" || f.name == "MotorShutter_PulseFwd") {
                    Fail(p + ".name", "acquisition lifecycle and shutter pulse writes are owned by the driver; "
                         "features.raw must not bypass or duplicate shutter preparation");
                }
                ReadEnum(item, p, "type", {"int", "float", "bool", "enum", "string", "command"}, f.type);
                if (f.type.empty()) {
                    Fail(p + ".type", "is required (int | float | bool | enum | string | command)");
                }
                const bool has_value = Present(item, "value");
                if (f.type != "command" && !has_value) {
                    Fail(p + ".value", "is required for type '" + f.type + "'");
                }
                // Convert here so the eBUS layer never parses text
                if (f.type == "int") {
                    f.int_value = Require<std::int64_t>(item, p, "value");
                } else if (f.type == "float") {
                    f.float_value = Require<double>(item, p, "value");
                } else if (f.type == "bool") {
                    f.bool_value = Require<bool>(item, p, "value");
                } else if (has_value && !Child(item, "value").IsNull()) {
                    f.value = Require<std::string>(item, p, "value");
                }
                features.raw.push_back(std::move(f));
            }
        }

        void ParseNetwork(const YAML::Node &root, NetworkConfig &net) {
            const YAML::Node n = OptionalMap(root, "", "network",
                                             {
                                                 "packet_size", "socket_rx_buffer_mb", "buffer_count", "stall_budget_s",
                                                 "max_buffer_memory_mb", "retrieve_timeout_ms"
                                             });
            const std::string p = "network";
            ReadRange<int>(n, p, "packet_size", 0, 65535, net.packet_size);
            ReadRange<int>(n, p, "socket_rx_buffer_mb", 1, 1024, net.socket_rx_buffer_mb);
            const YAML::Node bc = Child(n, "buffer_count");
            if (bc.IsDefined() && bc.IsScalar() && bc.Scalar() == "auto") {
                net.buffer_count = 0;
            } else {
                ReadRange<int>(n, p, "buffer_count", 0, 100000, net.buffer_count);
            }
            ReadRange<double>(n, p, "stall_budget_s", 0.0, 3600.0, net.stall_budget_s);
            ReadRange<int>(n, p, "max_buffer_memory_mb", 1, 1024 * 1024, net.max_buffer_memory_mb);
            // The acquisition loop's poll period; it also bounds how long Stop() waits for that thread
            ReadRange<int>(n, p, "retrieve_timeout_ms", 1, 10000, net.retrieve_timeout_ms);
        }

        void ParseOutput(const YAML::Node &root, RecordingConfig &rec) {
            const YAML::Node n = OptionalMap(root, "", "output",
                                             {
                                                 "rotation", "flush_interval_mb", "on_gap", "wavelengths",
                                                 "max_duration_s", "max_frames"
                                             });
            const std::string p = "output";
            const YAML::Node rot = OptionalMap(n, p, "rotation", {"max_lines", "max_mb"});
            Read(rot, p + ".rotation", "max_lines", rec.rotation.max_lines);
            Read(rot, p + ".rotation", "max_mb", rec.rotation.max_mb);
            ReadRange<std::uint32_t>(n, p, "flush_interval_mb", 0, 4096, rec.flush_interval_mb);
            ReadEnum<GapPolicy>(n, p, "on_gap", {{"record", GapPolicy::kRecord}, {"pad_zero", GapPolicy::kPadZero}},
                                rec.on_gap);

            const YAML::Node w = OptionalMap(n, p, "wavelengths", {
                                                 "source", "file", "list", "grid", "fwhm_list", "calibration"
                                             });
            const std::string wp = p + ".wavelengths";
            ReadEnum<WavelengthSource>(w, wp, "source",
                                       {
                                           {"none", WavelengthSource::kNone}, {"file", WavelengthSource::kFile},
                                           {"list", WavelengthSource::kList},
                                           {"grid", WavelengthSource::kGrid}
                                       },
                                       rec.wavelengths.source);
            Read(w, wp, "file", rec.wavelengths.file);
            ReadSequence(w, wp, "list", rec.wavelengths.list);
            const YAML::Node g = OptionalMap(w, wp, "grid", {"start_nm", "end_nm"});
            Read(g, wp + ".grid", "start_nm", rec.wavelengths.grid_start_nm);
            Read(g, wp + ".grid", "end_nm", rec.wavelengths.grid_end_nm);
            ReadSequence(w, wp, "fwhm_list", rec.wavelengths.fwhm_list);
            const YAML::Node cal = OptionalMap(w, wp, "calibration",
                                               {
                                                   "reference", "device_serial", "samples", "bands", "offset_x",
                                                   "offset_y"
                                               });
            auto &profile = rec.wavelengths.calibration;
            Read(cal, wp + ".calibration", "reference", profile.reference);
            Read(cal, wp + ".calibration", "device_serial", profile.device_serial);
            ReadRange<int>(cal, wp + ".calibration", "samples", 1, 65535, profile.samples);
            ReadRange<int>(cal, wp + ".calibration", "bands", 1, 65535, profile.bands);
            ReadRange<int>(cal, wp + ".calibration", "offset_x", 0, 65535, profile.offset_x);
            ReadRange<int>(cal, wp + ".calibration", "offset_y", 0, 65535, profile.offset_y);
            if (!(rec.wavelengths.grid_start_nm < rec.wavelengths.grid_end_nm)) {
                Fail(wp + ".grid", "start_nm must be < end_nm");
            }

            ReadRange<double>(n, p, "max_duration_s", 0.0, 86400.0 * 7, rec.max_duration_s);
            Read(n, p, "max_frames", rec.max_frames);
        }

        AppConfig ParseRoot(const YAML::Node &root) {
            CheckKeys(root, "", {
                          "device", "acquisition", "sensor_trigger", "features", "output", "network", "logging", "reference"
                      });
            AppConfig c;

            const YAML::Node d = OptionalMap(root, "", "device", {"mac", "ip"});
            Read(d, "device", "mac", c.device.mac);
            Read(d, "device", "ip", c.device.ip);
            if (!c.device.mac.empty() && common::StringUtil::NormalizeMac(c.device.mac).empty()) {
                Fail("device.mac", "is not a valid MAC address");
            }

            ParseAcquisition(root, c.acquisition);
            ParseSensorTrigger(root, c.sensor_trigger, c.acquisition);
            ParseFeatures(root, c.features);
            ParseNetwork(root, c.network);
            ParseOutput(root, c.recording);

            const YAML::Node reference = OptionalMap(root, "", "reference", {"duration_s"});
            ReadRange<double>(reference, "reference", "duration_s", 0.1, 3600.0, c.reference.duration_s);
            if (!std::isfinite(c.reference.duration_s)) Fail("reference.duration_s", "must be finite");

            const YAML::Node logging = OptionalMap(root, "", "logging", {"stats_interval_s"});
            ReadRange<double>(logging, "logging", "stats_interval_s", 0.0, 3600.0, c.logging.stats_interval_s);
            return c;
        }
    } // namespace

    std::vector<std::pair<int, int> > ParseMroiRegions(const std::string &multiband_string) {
        constexpr int kMaxRegions = 512; // manual p.26
        constexpr int kSensorRows = 1082; // physical sensor (p.7); the start row is a native row index
        constexpr int kMaxWindowHeight = 448; // readable window; caps each region and the total (p.26)

        std::vector<std::pair<int, int> > regions;
        std::istringstream stream(multiband_string);
        std::string token;
        while (std::getline(stream, token, ';')) {
            if (token.find_first_not_of(" \t") == std::string::npos) {
                continue; // trailing ';'
            }
            std::istringstream pair_stream(token);
            int y = -1;
            int h = -1;
            std::string extra;
            if (!(pair_stream >> y >> h) || (pair_stream >> extra)) {
                throw ConfigError("MROI region '" + token + "': expected '<start_row> <height>'");
            }
            if (y < 0 || y > kSensorRows - 1) {
                throw ConfigError("MROI region '" + token + "': start row must be in [0, 1081] (native sensor row; "
                                  "the usable range is Window.Y .. Window.Y+447, manual p.26)");
            }
            if (h < 1 || h > kMaxWindowHeight) {
                throw ConfigError("MROI region '" + token + "': height must be in [1, 448] (manual p.26)");
            }
            if (y + h > kSensorRows) {
                throw ConfigError("MROI region '" + token + "': start_row + height must not exceed 1082");
            }
            regions.emplace_back(y, h);
        }
        if (regions.empty()) {
            throw ConfigError("MROI multiband string contains no regions");
        }
        if (static_cast<int>(regions.size()) > kMaxRegions) {
            throw ConfigError("MROI supports at most 512 regions, got " + std::to_string(regions.size()));
        }
        int total = 0;
        for (const auto &[y, h]: regions) {
            total += h;
        }
        if (total > kMaxWindowHeight) {
            throw ConfigError("MROI total height " + std::to_string(total) +
                              " exceeds the FX10 readable window of 448 rows (manual p.26)");
        }
        return regions;
    }


    int AcquisitionConfig::ExpectedBands() const {
        if (mroi.enabled) {
            int total = 0;
            for (const auto &[y, h]: ParseMroiRegions(mroi.multiband_string)) {
                total += h;
            }
            return total;
        }
        return 448 / spectral_binning;
    }


    AppConfig LoadAppConfig(const std::string &path) {
        return ParseRoot(LoadFile(path));
    }


    AppConfig LoadAppConfigText(const std::string &yaml_text) {
        return ParseRoot(LoadText(yaml_text));
    }
} // namespace fx10
