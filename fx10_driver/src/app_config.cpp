#include "app_config.h"

#include <cmath>
#include <cstdio>

#include "sensor_sync_rates.h"
#include "string_util.h"


namespace fx10 {
    namespace {
        using namespace common::ConfigUtil;

        void ParseAcquisition(const YAML::Node &root, AcquisitionConfig &acq) {
            const YAML::Node n = OptionalMap(root, "", "acquisition",
                                             {
                                                 "spatial_binning", "spectral_binning", "pixel_format", "exposure_ms",
                                                 "frame_rate_hz", "trigger"
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

            const auto is_binning = [](int v) { return v == 1 || v == 2 || v == 4 || v == 8; };
            if (!is_binning(acq.spectral_binning)) {
                Fail(p + ".spectral_binning", "must be 1, 2, 4, or 8");
            }
            if (!is_binning(acq.spatial_binning)) {
                Fail(p + ".spatial_binning", "must be 1, 2, 4, or 8");
            }
            CheckAcquisitionTiming(acq);
        }

        void ParseSensorTrigger(const YAML::Node &root, SensorTriggerConfig &st, const AcquisitionConfig &acq) {
            // The port is the rig's (config-main.yaml "Sensor Trigger: Port")
            const YAML::Node n = OptionalMap(root, "", "sensor_trigger", {"enabled", "trigger_channel"});
            Read(n, "sensor_trigger", "enabled", st.enabled);
            ReadRange<int>(n, "sensor_trigger", "trigger_channel", 0, 3, st.trigger_channel);
            // The board drives paired outputs (0/1 = FX, 2/3 = JAI) at one rate per pair and
            // answers a rate its PWM cannot produce with #ERR,bad_freq (sensor_sync_rates.h)
            namespace ss = common::sensor_sync;
            const ss::Group group = ss::GroupOf(st.trigger_channel);
            if (st.enabled && acq.trigger.mode == TriggerMode::kExternal &&
                !ss::RateAccepted(group, acq.frame_rate_hz)) {
                Fail("acquisition.frame_rate_hz", "must be within " + ss::RangeText(group) + " for the SensorSync " +
                                                  ss::Name(group) + " group (outputs " + ss::Channels(group) + ")");
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
                          "device", "acquisition", "sensor_trigger", "features", "output", "network", "logging",
                          "reference"
                      });
            AppConfig c;

            const YAML::Node d = OptionalMap(root, "", "device", {"mac", "ip", "ebus_config"});
            Read(d, "device", "mac", c.device.mac);
            Read(d, "device", "ip", c.device.ip);
            Read(d, "device", "ebus_config", c.device.ebus_config);
            if (!c.device.ebus_config.empty()) {
                const std::filesystem::path pvcfg(c.device.ebus_config);
                if (pvcfg.extension() != ".pvcfg" || pvcfg.parent_path().filename().empty()) {
                    Fail("device.ebus_config", "must be '<DeviceSerialNumber>/<name>.pvcfg' (got '" +
                                               c.device.ebus_config + "')");
                }
            }
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

    void CheckAcquisitionTiming(const AcquisitionConfig &acq) {
        using common::ConfigUtil::Fail;
        const auto fmt = [](double value) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.3f", value);
            return std::string(text);
        };
        if (!std::isfinite(acq.exposure_ms) || !(acq.exposure_ms > 0.0)) {
            Fail("acquisition.exposure_ms", "must be finite and > 0");
        }
        if (acq.exposure_ms > kMaxExposureMs) {
            Fail("acquisition.exposure_ms", "must be <= " + fmt(kMaxExposureMs) + " ms (FX10e ExposureTime maximum)");
        }
        const double max_rate = MaxFrameRateHz(acq.spatial_binning, acq.spectral_binning);
        if (!std::isfinite(acq.frame_rate_hz) || !(acq.frame_rate_hz > 0.0)) {
            Fail("acquisition.frame_rate_hz", "must be finite and > 0");
        }
        if (acq.frame_rate_hz > max_rate) {
            Fail("acquisition.frame_rate_hz", "must be <= " + fmt(max_rate) + " Hz at " +
                                              std::to_string(acq.spatial_binning) + " x " +
                                              std::to_string(acq.spectral_binning) + " binning (" +
                                              fmt(kMaxFrameRateHzUnbinned) + " Hz at 1 x 1, " +
                                              fmt(kMaxFrameRateHzBinned) + " Hz otherwise)");
        }
        if (!ExposureFitsPeriod(acq.exposure_ms, acq.frame_rate_hz)) {
            Fail("acquisition.exposure_ms/.frame_rate_hz",
                 "exposure_ms (" + fmt(acq.exposure_ms) + ") must not exceed 90 % of the frame period (" +
                 fmt(ExposureLimitMs(acq.frame_rate_hz)) + " ms at " + fmt(acq.frame_rate_hz) +
                 " Hz); lower the exposure or the frame rate");
        }
    }


    int AcquisitionConfig::ExpectedBands() const {
        return 448 / spectral_binning;
    }


    AppConfig LoadAppConfig(const std::string &path) {
        return ParseRoot(LoadFile(path));
    }


    AppConfig LoadAppConfigText(const std::string &yaml_text) {
        return ParseRoot(LoadText(yaml_text));
    }
} // namespace fx10
