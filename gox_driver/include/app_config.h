#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config_util.h"


// YAML-backed configuration (config/config-gox.yaml). Strict schema: unknown
// keys are errors, omitted keys keep the defaults below. cameras[] carries one
// device/acquisition/features/network block per camera; output/ptp/logging are global.
namespace gox {
    using ConfigError = common::ConfigError;

    // The driver never writes network settings; re-addressing is the GUI's ebus_set_ip.
    struct DeviceConfig {
        std::string mac; // discovery match; wins over ip
        std::string ip; // direct connect; used when mac is empty
    };

    struct RoiConfig {
        uint32_t width = 0; // 0 = keep the device value
        uint32_t height = 0;
        uint32_t offset_x = 0;
        uint32_t offset_y = 0;
    };

    enum class TriggerMode { kFreerun, kExternal };

    enum class TriggerActivation { kRising, kFalling };

    struct TriggerConfig {
        TriggerMode mode = TriggerMode::kFreerun;
        TriggerActivation activation = TriggerActivation::kRising;
        std::string selector_entry = "FrameStart"; // only FrameStart drives per-frame exposure (manual p.42)
        // TriggerSource: a number is written as the enum's integer value, a name as the entry name.
        // 24 = Line5 Opt In, the only opto-coupled input and the factory default (manual p.141)
        std::string source_entry = "24";
    };

    struct AcquisitionConfig {
        std::optional<double> exposure_ms; // ExposureTime is in us (x1000)
        std::optional<double> gain; // Gain[AnalogAll] magnification x1.0..126.0 (manual p.148), not dB
        std::optional<double> frame_rate_hz; // freerun only
        std::optional<std::string> pixel_format; // GenICam enum entry, verbatim
        std::optional<RoiConfig> roi;
        // BlemishEnable (manual p.156): false = "Disable all", the sensor's own pixels, hot pixels included.
        // Only written when false; true is the factory value.
        bool blemish_correction = false;
        TriggerConfig trigger;
    };

    struct RawFeature {
        std::string name;
        std::string value; // scalar text; typed by the camera's node type
        bool value_is_string = false; // true when the YAML value was quoted
    };

    struct FeatureConfig {
        std::vector<RawFeature> raw; // applied in order, after the acquisition block
    };

    struct NetworkConfig {
        uint32_t buffer_count = 0; // 0 = auto (buffering.hpp)
        uint32_t packet_size = 0; // 0 = NegotiatePacketSize, fallback 1476
        uint32_t socket_rx_buffer_mb = 16;
        std::string local_ip; // bind the stream to a NIC; empty = auto
        uint32_t throughput_safety_margin_pct = 0; // NetworkThroughputSafetyMargin (manual p.128); 0 = factory 92
        std::vector<RawFeature> receiver_tuning; // PvStream parameters
    };

    struct CameraConfig {
        std::string id;
        bool enabled = true;
        DeviceConfig device;
        AcquisitionConfig acquisition;
        FeatureConfig features;
        NetworkConfig network;
    };

    enum class QueueOnFull { kDropNewest, kBlock };

    enum class OnBufferError { kRecordFlagged, kDrop };

    struct OutputConfig {
        uint32_t segment_size_mb = 2048;
        uint32_t record_align = 4096; // power of two; 1 = no alignment
        uint32_t queue_max_frames = 0; // 0 = auto; pool allocates +2 chunks
        QueueOnFull queue_on_full = QueueOnFull::kDropNewest;
        OnBufferError on_buffer_error = OnBufferError::kRecordFlagged;
        uint32_t flush_interval_mb = 64; // sync_file_range cadence
        uint64_t max_frames = 0; // 0 = unlimited
        double max_duration_s = 0; // 0 = unlimited
    };

    enum class PtpOnTimeout { kAbort, kWarnContinue };

    // GevIEEE1588 / GevIEEE1588Status (manual p.121/p.128)
    struct PtpConfig {
        bool enabled = false;
        double sync_timeout_s = 60.0; // budget to reach the "slave" state
        PtpOnTimeout on_timeout = PtpOnTimeout::kAbort;
        double offset_report_interval_s = 60.0; // 0 = only at session start/end
    };

    struct AppConfig {
        std::vector<CameraConfig> cameras;
        OutputConfig output;
        PtpConfig ptp;
        double stats_interval_s = 2.5; // > 0
    };

    // Throws ConfigError.
    AppConfig LoadAppConfig(const std::string &path);

    AppConfig LoadAppConfigText(const std::string &yaml_text);
} // namespace gox
