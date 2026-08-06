#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


namespace fx10 {
    class ConfigError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // ---------------------------------------------------------------------------
    struct ForceIpConfig {
        bool enabled = false; // FORCEIP rescue for a wrong-subnet camera; requires device.mac
        std::string ip; // temporary address to force (lost on power cycle)
        std::string subnet_mask;
        std::string gateway = "0.0.0.0";
    };

    struct DeviceConfig {
        std::string id; // eBUS connection ID / device user ID; takes priority when set
        std::string mac; // used when id is empty: discovery match, works even with a bad IP config
        std::string ip; // used when id and mac are empty
        ForceIpConfig force_ip;
    };

    struct ReconnectConfig {
        bool enabled = true;
        int max_attempts = 5;
        int backoff_ms = 2000;
    };

    struct NetworkConfig {
        int packet_size = 0; // 0 = NegotiatePacketSize (jumbo if the path allows)
        int socket_rx_buffer_mb = 32; // PvStreamGEV::SetUserModeSocketRxBufferSize
        int buffer_count = 0; // 0 = auto: ceil(fps * stall_budget_s) + 16
        double stall_budget_s = 2.0; // disk-stall absorption target used by auto sizing
        int max_buffer_memory_mb = 512; // clamp for the auto-sized buffer pool
        int retrieve_timeout_ms = 1000;
        ReconnectConfig reconnect;
    };

    enum class TriggerMode { kExternal, kFreerun };

    enum class TriggerActivation { kRising, kFalling };

    enum class ExposureControl { kCamera, kPulseWidth };

    struct TriggerConfig {
        TriggerMode mode = TriggerMode::kExternal;
        TriggerActivation activation = TriggerActivation::kRising;
        double delay_ms = 0.0; // FX10 trigger delay unit is milliseconds
        ExposureControl exposure_control = ExposureControl::kCamera;
        // Enum ENTRY VALUES written to the trigger selector/source nodes (verified
        // against docs/fx10_features_dump.txt).
        std::string selector_entry = "FrameStart";
        std::string source_entry = "Line0";
    };

    struct MroiConfig {
        bool enabled = false;
        std::string multiband_string; // "y1 h1;y2 h2;..." written verbatim to the camera
    };

    // Teensy SensorSync-Logger (submodule/sensor_trigger): fires the camera's
    // hardware trigger pulses and records the raw timing log into the session
    // directory (sensor_trigger.log) — the sole time source for recorded lines.
    // The pulse rate is NOT configured on the board: every session start sends
    // acquisition.frame_rate_hz for trigger_channel (board minimum 1 Hz), and
    // freerun sends 0 = channel off; other channels keep their config.h rates.
    struct SensorTriggerConfig {
        bool enabled = false;
        std::string port; // /dev/serial/by-id/... ; required when enabled
        int trigger_channel = 0; // Teensy trig[N] wired to the camera's trigger input
    };

    struct AcquisitionConfig {
        int spatial_binning = 1; // [1|2|4|8] -> 1024/512/256/128 pixel width
        int spectral_binning = 1; // [1|2|4|8] -> 448/224/112/56 bands
        std::string pixel_format = "Mono12Packed"; // [Mono12Packed | Mono12 | Mono10Packed | Mono10 | Mono8]
        double exposure_ms = 5.0;
        // Freerun frame rate, under external trigger it doubles as the EXPECTED pulse rate used for buffer-pool sizing
        double frame_rate_hz = 50.0;
        MroiConfig mroi;
        bool status_line = false; // when on, the camera overwrites the LAST image row
        TriggerConfig trigger;

        // Expected spectral band count implied by this configuration
        int expectedBands() const;
    };

    enum class GapPolicy { kRecord, kPadZero };

    enum class WavelengthSource { kFile, kList, kGrid };

    struct WavelengthConfig {
        WavelengthSource source = WavelengthSource::kGrid;
        std::string file; // calibration-pack text file
        std::vector<double> list; // explicit nm values
        double grid_start_nm = 400.0;
        double grid_end_nm = 1000.0;
        std::vector<double> fwhm_list; // optional; empty = omit fwhm from the .hdr
    };

    struct RotationConfig {
        std::uint64_t max_lines = 100000; // 0 = no rotation by line count
        std::uint64_t max_megabytes = 0; // 0 = no rotation by size
    };

    struct RecordingConfig {
        std::string output_dir = "./captures"; // overridden to <data>/bin/fx10 by Fx10DriverApp
        std::string base_name = "fx10";
        RotationConfig rotation;
        GapPolicy on_gap = GapPolicy::kRecord;
        WavelengthConfig wavelengths;
        double max_duration_s = 0.0; // stop the session after this many seconds (0 = unlimited)
        std::uint64_t max_frames = 0; // stop after this many delivered frames (0 = unlimited)
    };

    struct DiskConfig {
        double min_free_gb = 2.0; // hard floor: clean stop + finalize below this
        double warn_free_gb = 5.0; // soft floor: WARN below this
    };

    struct WatchdogConfig {
        double no_frame_warn_s = 5.0;
        // -1 = auto (resolved from trigger mode: external -> 0 = never abort, since silence just means the pulses stopped; freerun -> 10)
        double no_frame_abort_s = -1.0;
    };

    struct RawFeature {
        std::string name;
        std::string type; // int | float | bool | enum | string | command
        std::string value; // ignored for command
    };

    struct FeatureConfig {
        std::map<std::string, std::string> map = defaultMap();
        std::vector<RawFeature> raw; // applied in order, after the structured settings

        static const std::vector<std::string> &knownRoles();

        static std::map<std::string, std::string> defaultMap();

        // Resolved node name for a role ("" = not mapped). Throws on unknown role
        const std::string &node(const std::string &role) const;
    };

    struct LogConfig {
        double stats_interval_s = 2.5;
    };


    // ---------------------------------------------------------------------------
    struct Config {
        DeviceConfig device;
        NetworkConfig network;
        AcquisitionConfig acquisition;
        SensorTriggerConfig sensor_trigger;
        RecordingConfig recording;
        DiskConfig disk;
        WatchdogConfig watchdog;
        FeatureConfig features;
        LogConfig logging;

        // Watchdog abort with the auto default resolved against the trigger mode.
        double resolvedNoFrameAbortS() const;

        static Config loadFromFile(const std::string &path);

        static Config loadFromString(const std::string &yaml_text, const std::string &origin = "<string>");
    };

    // "y1 h1;y2 h2;..." -> list of (start_row, height)
    std::vector<std::pair<int, int> > parseMroiRegions(const std::string &multiband_string);
} // namespace fx10
