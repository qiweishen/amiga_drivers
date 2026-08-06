#include "app_config.hpp"

#include <sstream>
#include <yaml-cpp/yaml.h>

#include "string_util.h"
#include "utility.h"


namespace fx10 {
    namespace {
        TriggerMode read_trigger_mode(const YAML::Node &n) {
            const auto text = n.as<std::string>();
            if (text == "external") {
                return TriggerMode::kExternal;
            }
            if (text == "freerun") {
                return TriggerMode::kFreerun;
            }
            throw ConfigError("acquisition.trigger.mode must be 'external' or 'freerun'");
        }


        TriggerActivation read_trigger_activation(const YAML::Node &n) {
            const auto text = n.as<std::string>();
            if (text == "rising") {
                return TriggerActivation::kRising;
            }
            if (text == "falling") {
                return TriggerActivation::kFalling;
            }
            throw ConfigError("acquisition.trigger.activation must be 'rising' or 'falling'");
        }


        ExposureControl read_exposure_control(const YAML::Node &n) {
            const auto text = n.as<std::string>();
            if (text == "camera") {
                return ExposureControl::kCamera;
            }
            if (text == "pulse_width") {
                return ExposureControl::kPulseWidth;
            }
            throw ConfigError("acquisition.trigger.exposure_control must be 'camera' or 'pulse_width'");
        }


        GapPolicy read_gap_policy(const YAML::Node &n) {
            const auto text = n.as<std::string>();
            if (text == "record") {
                return GapPolicy::kRecord;
            }
            if (text == "pad_zero") {
                return GapPolicy::kPadZero;
            }
            throw ConfigError("recording.on_gap must be 'record' or 'pad_zero'");
        }


        WavelengthSource read_wavelength_source(const YAML::Node &n) {
            const auto text = n.as<std::string>();
            if (text == "file") {
                return WavelengthSource::kFile;
            }
            if (text == "list") {
                return WavelengthSource::kList;
            }
            if (text == "grid") {
                return WavelengthSource::kGrid;
            }
            throw ConfigError("recording.wavelengths.source must be 'file', 'list' or 'grid'");
        }


        // Accepts the literal "auto" (as documented in the templates) as well as 0
        int read_buffer_count(const YAML::Node &n) {
            if (n.IsScalar() && n.Scalar() == "auto") {
                return 0;
            }
            try {
                return n.as<int>();
            } catch (const YAML::Exception &) {
                throw ConfigError("network.buffer_count must be an integer or 'auto'");
            }
        }


        std::vector<double> read_double_seq(const YAML::Node &n) {
            std::vector<double> out;
            for (const auto &item: n) {
                out.push_back(item.as<double>());
            }
            return out;
        }


        Config parse_root(const YAML::Node &root, const std::string &origin) {
            Config c;
            if (!root.IsDefined() || root.IsNull()) {
                return c; // empty document = all defaults
            }

            try {
                if (auto n = root["device"]) {
                    if (n["id"]) {
                        c.device.id = n["id"].as<std::string>();
                    }
                    if (n["mac"]) {
                        c.device.mac = n["mac"].as<std::string>();
                    }
                    if (n["ip"]) {
                        c.device.ip = n["ip"].as<std::string>();
                    }
                    if (auto f = n["force_ip"]) {
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

                if (auto n = root["acquisition"]) {
                    if (n["spatial_binning"]) {
                        c.acquisition.spatial_binning = n["spatial_binning"].as<int>();
                    }
                    if (n["spectral_binning"]) {
                        c.acquisition.spectral_binning = n["spectral_binning"].as<int>();
                    }
                    if (n["pixel_format"]) {
                        c.acquisition.pixel_format = n["pixel_format"].as<std::string>();
                    }
                    if (n["exposure_ms"]) {
                        c.acquisition.exposure_ms = n["exposure_ms"].as<double>();
                    }
                    if (n["frame_rate_hz"]) {
                        c.acquisition.frame_rate_hz = n["frame_rate_hz"].as<double>();
                    }
                    if (n["status_line"]) {
                        c.acquisition.status_line = n["status_line"].as<bool>();
                    }
                    if (auto t = n["trigger"]) {
                        if (t["mode"]) {
                            c.acquisition.trigger.mode = read_trigger_mode(t["mode"]);
                        }
                        if (t["activation"]) {
                            c.acquisition.trigger.activation = read_trigger_activation(t["activation"]);
                        }
                        if (t["delay_ms"]) {
                            c.acquisition.trigger.delay_ms = t["delay_ms"].as<double>();
                        }
                        if (t["exposure_control"]) {
                            c.acquisition.trigger.exposure_control = read_exposure_control(t["exposure_control"]);
                        }
                        if (t["selector_entry"]) {
                            c.acquisition.trigger.selector_entry = t["selector_entry"].as<std::string>();
                        }
                        if (t["source_entry"]) {
                            c.acquisition.trigger.source_entry = t["source_entry"].as<std::string>();
                        }
                    }
                    if (auto m = n["mroi"]) {
                        if (m["enabled"]) {
                            c.acquisition.mroi.enabled = m["enabled"].as<bool>();
                        }
                        if (m["multiband_string"]) {
                            c.acquisition.mroi.multiband_string = m["multiband_string"].as<std::string>();
                        }
                    }
                }

                if (auto n = root["sensor_trigger"]) {
                    if (n["enabled"]) {
                        c.sensor_trigger.enabled = n["enabled"].as<bool>();
                    }
                    if (n["port"]) {
                        c.sensor_trigger.port = n["port"].as<std::string>();
                    }
                    if (n["trigger_channel"]) {
                        c.sensor_trigger.trigger_channel = n["trigger_channel"].as<int>();
                    }
                }

                if (auto n = root["features"]) {
                    if (auto m = n["map"]; m && m.IsMap()) {
                        for (const auto &item: m) {
                            c.features.map[item.first.as<std::string>()] = item.second.IsNull()
                                                                               ? ""
                                                                               : item.second.as<std::string>();
                        }
                    }
                    if (auto raw = n["raw"]; raw && raw.IsSequence()) {
                        for (const auto &item: raw) {
                            RawFeature feature;
                            if (item["name"]) {
                                feature.name = item["name"].as<std::string>();
                            }
                            if (item["type"]) {
                                feature.type = item["type"].as<std::string>();
                            }
                            if (item["value"]) {
                                feature.value = item["value"].as<std::string>();
                            }
                            c.features.raw.push_back(std::move(feature));
                        }
                    }
                }

                if (auto n = root["network"]) {
                    if (n["packet_size"]) {
                        c.network.packet_size = n["packet_size"].as<int>();
                    }
                    if (n["socket_rx_buffer_mb"]) {
                        c.network.socket_rx_buffer_mb = n["socket_rx_buffer_mb"].as<int>();
                    }
                    if (n["buffer_count"]) {
                        c.network.buffer_count = read_buffer_count(n["buffer_count"]);
                    }
                    if (n["stall_budget_s"]) {
                        c.network.stall_budget_s = n["stall_budget_s"].as<double>();
                    }
                    if (n["max_buffer_memory_mb"]) {
                        c.network.max_buffer_memory_mb = n["max_buffer_memory_mb"].as<int>();
                    }
                    if (n["retrieve_timeout_ms"]) {
                        c.network.retrieve_timeout_ms = n["retrieve_timeout_ms"].as<int>();
                    }
                    if (auto r = n["reconnect"]) {
                        if (r["enabled"]) {
                            c.network.reconnect.enabled = r["enabled"].as<bool>();
                        }
                        if (r["max_attempts"]) {
                            c.network.reconnect.max_attempts = r["max_attempts"].as<int>();
                        }
                        if (r["backoff_ms"]) {
                            c.network.reconnect.backoff_ms = r["backoff_ms"].as<int>();
                        }
                    }
                }

                if (auto n = root["output"]) {
                    if (n["output_dir"]) {
                        c.recording.output_dir = n["output_dir"].as<std::string>();
                    }
                    if (n["base_name"]) {
                        c.recording.base_name = n["base_name"].as<std::string>();
                    }
                    if (auto rot = n["rotation"]) {
                        if (rot["max_lines"]) {
                            c.recording.rotation.max_lines = rot["max_lines"].as<std::uint64_t>();
                        }
                        if (rot["max_megabytes"]) {
                            c.recording.rotation.max_megabytes = rot["max_megabytes"].as<std::uint64_t>();
                        }
                    }
                    if (n["on_gap"]) {
                        c.recording.on_gap = read_gap_policy(n["on_gap"]);
                    }
                    if (auto w = n["wavelengths"]) {
                        if (w["source"]) {
                            c.recording.wavelengths.source = read_wavelength_source(w["source"]);
                        }
                        if (w["file"]) {
                            c.recording.wavelengths.file = w["file"].as<std::string>();
                        }
                        if (w["list"]) {
                            c.recording.wavelengths.list = read_double_seq(w["list"]);
                        }
                        if (auto g = w["grid"]) {
                            if (g["start_nm"]) {
                                c.recording.wavelengths.grid_start_nm = g["start_nm"].as<double>();
                            }
                            if (g["end_nm"]) {
                                c.recording.wavelengths.grid_end_nm = g["end_nm"].as<double>();
                            }
                        }
                        if (w["fwhm_list"]) {
                            c.recording.wavelengths.fwhm_list = read_double_seq(w["fwhm_list"]);
                        }
                    }
                    if (n["max_duration_s"]) {
                        c.recording.max_duration_s = n["max_duration_s"].as<double>();
                    }
                    if (n["max_frames"]) {
                        c.recording.max_frames = n["max_frames"].as<std::uint64_t>();
                    }
                }

                if (auto n = root["disk"]) {
                    if (n["min_free_gb"]) {
                        c.disk.min_free_gb = n["min_free_gb"].as<double>();
                    }
                    if (n["warn_free_gb"]) {
                        c.disk.warn_free_gb = n["warn_free_gb"].as<double>();
                    }
                }

                if (auto n = root["watchdog"]) {
                    if (n["no_frame_warn_s"]) {
                        c.watchdog.no_frame_warn_s = n["no_frame_warn_s"].as<double>();
                    }
                    if (n["no_frame_abort_s"]) {
                        c.watchdog.no_frame_abort_s = n["no_frame_abort_s"].as<double>();
                    }
                }

                if (auto n = root["logging"]) {
                    if (n["stats_interval_s"]) {
                        c.logging.stats_interval_s = n["stats_interval_s"].as<double>();
                    }
                }

                if (c.acquisition.pixel_format != "Mono8" && c.acquisition.pixel_format != "Mono10" &&
                    c.acquisition.pixel_format != "Mono10Packed" && c.acquisition.pixel_format != "Mono12" &&
                    c.acquisition.pixel_format != "Mono12Packed") {
                    throw ConfigError(
                        "acquisition.pixel_format must be [Mono12Packed | Mono12 | Mono10Packed | Mono10 | Mono8]");
                }
                const int binning = c.acquisition.spectral_binning;
                if (binning != 1 && binning != 2 && binning != 4 && binning != 8) {
                    throw ConfigError("acquisition.spectral_binning must be 1, 2, 4, or 8");
                }
                if (c.acquisition.mroi.enabled) {
                    if (binning != 1) {
                        throw ConfigError("acquisition.mroi requires spectral_binning: 1 (FX10 constraint)");
                    }
                    parseMroiRegions(c.acquisition.mroi.multiband_string); // throws on bad syntax
                }
                if (!(c.acquisition.exposure_ms > 0.0)) {
                    throw ConfigError("acquisition.exposure_ms must be > 0");
                }
                if (!(c.acquisition.frame_rate_hz > 0.0)) {
                    throw ConfigError("acquisition.frame_rate_hz must be > 0");
                }
                if (c.sensor_trigger.enabled && c.sensor_trigger.port.empty()) {
                    throw ConfigError("sensor_trigger.port is required when sensor_trigger.enabled "
                        "(a /dev/serial/by-id/... path survives USB re-enumeration)");
                }
                if (c.sensor_trigger.trigger_channel < 0) {
                    throw ConfigError("sensor_trigger.trigger_channel must be >= 0 (Teensy trig[N] index)");
                }
                if (c.sensor_trigger.enabled && c.acquisition.trigger.mode == TriggerMode::kExternal &&
                    c.acquisition.frame_rate_hz < 1.0) {
                    // The board rejects sub-1-Hz trigger rates (#ERR,bad_freq) and would
                    // silently keep pulsing at the previous rate instead.
                    throw ConfigError("acquisition.frame_rate_hz must be >= 1 when the sensor_trigger drives "
                        "the external trigger (SensorSync board minimum)");
                }
                if (c.recording.output_dir.empty()) {
                    throw ConfigError("recording.output_dir must not be empty");
                }
                if (c.recording.base_name.empty() ||
                    c.recording.base_name.find_first_of("/\\ \t") != std::string::npos) {
                    throw ConfigError("recording.base_name must be non-empty and contain no slashes or whitespace");
                }
                if (!(c.recording.wavelengths.grid_start_nm < c.recording.wavelengths.grid_end_nm)) {
                    throw ConfigError("recording.wavelengths.grid start_nm must be < end_nm");
                }
                if (c.disk.warn_free_gb < c.disk.min_free_gb) {
                    throw ConfigError("disk.warn_free_gb must be >= disk.min_free_gb");
                }
                if (c.network.buffer_count < 0) {
                    throw ConfigError("network.buffer_count must be >= 0 (0 or 'auto' = auto)");
                }
                if (c.network.reconnect.max_attempts < 0 || c.network.reconnect.backoff_ms < 0) {
                    throw ConfigError("network.reconnect values must be >= 0");
                }
                if (!c.device.mac.empty() && Common::StringUtil::NormalizeMac(c.device.mac).empty()) {
                    throw ConfigError("device.mac is not a valid MAC address");
                }
                if (c.device.force_ip.enabled) {
                    // FORCEIP addresses the camera by MAC, before it has a usable IP
                    if (c.device.mac.empty()) {
                        throw ConfigError("device.force_ip requires device.mac");
                    }
                    if (c.device.force_ip.ip.empty() || c.device.force_ip.subnet_mask.empty()) {
                        throw ConfigError("device.force_ip.enabled requires ip and subnet_mask");
                    }
                }
            } catch (const ConfigError &) {
                throw;
            } catch (const std::exception &e) {
                throw ConfigError(std::string("invalid config '") + origin + "': " + e.what());
            }

            return c;
        }
    } // namespace


    // ---------------------------------------------------------------------------
    std::vector<std::pair<int, int> > parseMroiRegions(const std::string &multiband_string) {
        constexpr int kMaxRegions = 512;
        constexpr int kMaxTotalHeight = 1082;

        std::vector<std::pair<int, int> > regions;
        std::istringstream stream(multiband_string);
        std::string token;
        while (std::getline(stream, token, ';')) {
            if (token.find_first_not_of(" \t") == std::string::npos) {
                continue; // allow trailing ';'
            }
            std::istringstream pair_stream(token);
            int y = -1;
            int h = -1;
            std::string extra;
            if (!(pair_stream >> y >> h) || (pair_stream >> extra)) {
                throw ConfigError("MROI region '" + token + "': expected '<start_row> <height>'");
            }
            if (y < 0 || y > kMaxTotalHeight - 1) {
                throw ConfigError("MROI region '" + token + "': start row must be in [0, 1081]");
            }
            if (h < 1 || y + h > kMaxTotalHeight) {
                throw ConfigError("MROI region '" + token +
                                  "': height must be >= 1 and start_row + height <= 1082");
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
        for (const auto &[y, h]: regions) total += h;
        if (total > kMaxTotalHeight) {
            throw ConfigError("MROI total height " + std::to_string(total) + " exceeds 1082 rows");
        }
        return regions;
    }


    int AcquisitionConfig::expectedBands() const {
        if (mroi.enabled) {
            int total = 0;
            for (const auto &[y, h]: parseMroiRegions(mroi.multiband_string)) {
                total += h;
            }
            return total;
        }
        return 448 / spectral_binning;
    }


    // ---------------------------------------------------------------------------
    const std::vector<std::string> &FeatureConfig::knownRoles() {
        static const std::vector<std::string> kRoles = {
            "acquisition_mode", "exposure_time", "exposure_mode",
            "frame_rate", "frame_rate_enable", "pixel_format",
            "trigger_selector", "trigger_mode", "trigger_source",
            "trigger_activation", "trigger_delay", "spectral_binning",
            "mroi_enable", "mroi_index", "mroi_y",
            "mroi_h", "status_line", "missed_trigger_count",
            "missed_trigger_reset", "extended_id_mode",
            "device_temperature",
        };
        return kRoles;
    }


    std::map<std::string, std::string> FeatureConfig::defaultMap() {
        return {
            {"acquisition_mode", "AcquisitionMode"},
            {"exposure_time", "ExposureTime"}, // float, microseconds [10..419000]
            {"exposure_mode", "ExposureMode"}, // {Timed TriggerControlled}
            {"frame_rate", "AcquisitionFrameRate"}, // float [2.39..324.4]
            {"frame_rate_enable", "EnAcquisitionFrameRate"},
            {"pixel_format", "PixelFormat"}, // {Mono8 Mono10 Mono10Packed Mono12 Mono12Packed Mono16}
            {"trigger_selector", "TriggerSelector"},
            {"trigger_mode", "TriggerMode"},
            {"trigger_source", "TriggerSource"}, // Line0 = ISO_TRIGGER pin 8
            {"trigger_activation", "TriggerActivation"},
            {"trigger_delay", "TriggerDelay"}, // float, microseconds [0..419000]
            {"spectral_binning", "BinningVertical"}, // int [1..8]
            {"mroi_enable", "MROI_Enable"},
            {"mroi_index", "MROI_Index"}, // int [0..511]
            {"mroi_y", "MROI_Y"}, // int [0..1081] native sensor rows
            {"mroi_h", "MROI_H"}, // int [1..1082]
            {"status_line", "EnStatusLine"},
            {"missed_trigger_count", "Counter1_Value"},
            {"missed_trigger_reset", "Counter1_Reset"},
            {"extended_id_mode", ""}, // absent on FX10e: 16-bit BlockIDs
            {"device_temperature", "DeviceTemperature"},
        };
    }


    const std::string &FeatureConfig::node(const std::string &role) const {
        const auto it = map.find(role);
        if (it == map.end()) {
            throw ConfigError("unknown feature role '" + role + "'");
        }
        return it->second;
    }


    // ---------------------------------------------------------------------------
    double Config::resolvedNoFrameAbortS() const {
        if (watchdog.no_frame_abort_s >= 0.0) return watchdog.no_frame_abort_s;
        return acquisition.trigger.mode == TriggerMode::kExternal ? 0.0 : 10.0;
    }


    Config Config::loadFromFile(const std::string &path) {
        YAML::Node root;
        try {
            // Unified loading path (ConfigLoader logs and throws on failure)
            root = Common::ConfigLoader(path).root();
        } catch (const std::exception &e) {
            throw ConfigError(std::string("failed to load config '") + path + "': " + e.what());
        }
        return parse_root(root, path);
    }


    Config Config::loadFromString(const std::string &yaml_text, const std::string &origin) {
        YAML::Node root;
        try {
            root = YAML::Load(yaml_text);
        } catch (const YAML::ParserException &e) {
            throw ConfigError(origin + ": YAML parse error: " + e.what());
        }
        return parse_root(root, origin);
    }
} // namespace fx10
