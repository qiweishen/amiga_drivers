#pragma once

// YAML-backed configuration for the GoX driver (config/config-gox.yaml).
// Sections, names and parsing style mirror fx10_driver's app_config: lenient
// if-present parsing (absent keys keep the struct defaults, unknown keys are
// ignored), only invariants that would corrupt data or break bring-up throw.
// Multi-camera: cameras[] carries one device/acquisition/features/network
// block per camera; output/disk/ptp/logging are global.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>


namespace jai {
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
        std::string mac; // discovery match (survives a bad IP config); wins over ip
        std::string ip; // direct connect (fastest); used when mac is empty
        ForceIpConfig force_ip;
    };

    struct RoiConfig {
        uint32_t width = 0; // 0 = leave device value untouched
        uint32_t height = 0; // 0 = leave device value untouched
        uint32_t offset_x = 0;
        uint32_t offset_y = 0;
    };

    struct TriggerConfig {
        std::string mode = "freerun"; // freerun | external
        std::string activation = "rising"; // rising | falling
        std::string selector_entry = "FrameStart";
        std::string source_entry = "Line1";
    };

    struct AcquisitionConfig {
        std::optional<double> exposure_ms; // written to ExposureTime in µs (x1000)
        std::optional<double> gain; // GainSelector chain: AnalogAll -> All -> skip
        std::optional<double> frame_rate_hz; // freerun only
        std::optional<std::string> pixel_format; // GenICam enum entry, verbatim
        std::optional<RoiConfig> roi;
        TriggerConfig trigger;
    };

    struct RawFeature {
        std::string name;
        std::string value; // scalar rendered as text; typed by the CAMERA's node type
        bool value_is_string = false; // true when the YAML value was quoted
    };

    struct FeatureConfig {
        std::vector<RawFeature> raw; // applied in order, after the acquisition block
    };

    struct NetworkConfig {
        uint32_t channel = 0;
        uint32_t buffer_count = 0; // 0 = auto (frame_rate x 0.5s, clamped [16, 256])
        uint32_t packet_size = 0; // 0 = NegotiatePacketSize, fallback 1476
        uint32_t socket_rx_buffer_mb = 16;
        std::string local_ip; // bind stream to a specific NIC; empty = auto
        uint32_t gev_scpd_ticks = 0; // inter-packet delay (bandwidth partitioning)
        std::vector<RawFeature> receiver_tuning; // escape hatch on PvStream params
    };

    struct CameraConfig {
        std::string id;
        bool enabled = true;
        DeviceConfig device;
        AcquisitionConfig acquisition;
        FeatureConfig features;
        NetworkConfig network;
    };

    // ---------------------------------------------------------------------------
    struct OutputConfig {
        std::string output_dir = "/data/captures"; // overridden to <data>/bin/gox by the unified runtime
        double segment_size_gib = 2.0;
        uint32_t record_align = 4096; // power of two; 1 = no alignment
        uint32_t queue_max_frames = 32; // queue capacity; pool allocates +2 chunks
        std::string queue_on_full = "drop_newest"; // drop_newest | block
        std::string on_buffer_error = "record_flagged"; // record_flagged | drop
        uint32_t flush_interval_mb = 64; // sync_file_range cadence
        uint64_t max_frames = 0; // stop after this many recorded frames (0 = unlimited)
        double max_duration_s = 0; // stop the session after this many seconds (0 = unlimited)
    };

    struct DiskConfig {
        double min_free_gb = 10.0; // orderly stop below this free space
    };

    struct WatchdogConfig {
        double no_frame_warn_s = 5.0;
        // -1 = auto (resolved from the camera's trigger mode: external -> 0 =
        // never abort, since silence just means the pulses stopped; freerun -> 10)
        double no_frame_abort_s = -1.0;

        // Auto default resolved against a camera's trigger mode (fx10 rule).
        double resolved_no_frame_abort_s(const std::string &trigger_mode) const {
            if (no_frame_abort_s >= 0.0) {
                return no_frame_abort_s;
            }
            return trigger_mode == "external" ? 0.0 : 10.0;
        }
    };

    struct PtpConfig {
        bool enabled = false; // off unless the config asks for it
        std::string feature_set = "auto"; // auto | gev_ieee1588 | sfnc_ptp | explicit
        std::string enable_feature; // used when feature_set == "explicit"
        std::string status_feature; // used when feature_set == "explicit"
        std::string required_status = "Slave";
        double sync_timeout_s = 60.0;
        uint32_t poll_interval_ms = 500;
        std::string on_timeout = "abort"; // abort | warn_continue
        double offset_report_interval_s = 60.0; // 0 = only at session start/end
        bool expect_tai_offset = true;
    };

    // ---------------------------------------------------------------------------
    struct AppConfig {
        std::vector<CameraConfig> cameras;
        OutputConfig output;
        DiskConfig disk;
        WatchdogConfig watchdog;
        PtpConfig ptp;
        double stats_interval_s = 5.0; // periodic stats line interval, must be > 0
    };

    // Parses and validates a YAML config file (unified loading path via
    // Common::ConfigLoader). Throws ConfigError on any problem.
    AppConfig load_config(const std::string &path);

    // Same, from YAML text (unit tests).
    AppConfig load_config_text(const std::string &yaml_text);
} // namespace jai
