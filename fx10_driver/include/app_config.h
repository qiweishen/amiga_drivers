#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "config_util.h"


// YAML-backed configuration (config/config-fx10.yaml). Strict schema: unknown
// keys are errors, omitted keys keep the defaults below.
namespace fx10 {
    using ConfigError = common::ConfigError;

    struct DeviceConfig {
        std::string mac; // discovery match; wins over ip
        std::string ip; // direct connect; used when mac is empty
    };

    struct NetworkConfig {
        int packet_size = 0; // 0 = NegotiatePacketSize
        int socket_rx_buffer_mb = 32; // PvStreamGEV::SetUserModeSocketRxBufferSize
        int buffer_count = 0; // 0 = 16 SDK receive buffers; explicit count remains subject to hard caps
        double stall_budget_s = 2.0; // owned application queue target: ceil(fps * budget), at least one frame
        int max_buffer_memory_mb = 512; // combined SDK/application payload buffers + unpack scratch; excludes bookkeeping
        int retrieve_timeout_ms = 1000;
    };

    enum class TriggerMode { kExternal, kFreerun };

    enum class TriggerActivation { kRising, kFalling };

    enum class ExposureControl { kCamera, kPulseWidth };

    struct TriggerConfig {
        TriggerMode mode = TriggerMode::kExternal;
        TriggerActivation activation = TriggerActivation::kRising;
        double delay_ms = 0.0;
        ExposureControl exposure_control = ExposureControl::kCamera;
        std::string selector_entry = "FrameStart";
        std::string source_entry = "Line0";
    };

    struct MroiConfig {
        bool enabled = false;
        std::string multiband_string; // "y1 h1;y2 h2;..." native sensor rows
    };

    // The board's serial port is the rig's (config-main.yaml "Sensor Trigger: Port")
    struct SensorTriggerConfig {
        bool enabled = false;
        int trigger_channel = 0; // physical log ID; v2 pairs 0/1 = FX, 2/3 = JAI
    };

    struct AcquisitionConfig {
        int spatial_binning = 1; // 1|2|4|8; actual width comes from device readback
        int spectral_binning = 2; // 1|2|4|8; actual height comes from device readback, not the optical-band estimate
        std::string pixel_format = "Mono12Packed";
        double exposure_ms = 5.0;
        double frame_rate_hz = 50.0; // freerun rate; expected pulse rate under external trigger
        MroiConfig mroi;
        bool status_line = false; // on: the camera overwrites the last image row
        bool image_enhancement = true; // AIE; only true is supported (node name unknown)
        TriggerConfig trigger;

        int ExpectedBands() const;
    };

    enum class GapPolicy { kRecord, kPadZero };

    enum class WavelengthSource { kNone, kFile, kList, kGrid };

    struct CalibrationConfig {
        std::string reference; // calibration pack / immutable revision, supplied by the operator
        std::string device_serial;
        int samples = 0;
        int bands = 0;
        int offset_x = -1;
        int offset_y = -1;
    };

    struct WavelengthConfig {
        WavelengthSource source = WavelengthSource::kNone; // uncalibrated: omit the wavelength axis
        CalibrationConfig calibration;
        std::string file; // calibration-pack text file
        std::vector<double> list; // explicit nm values
        double grid_start_nm = 400.0;
        double grid_end_nm = 1000.0;
        std::vector<double> fwhm_list; // empty = omit fwhm from the .hdr
    };

    struct RotationConfig {
        std::uint64_t max_lines = 0; // 0 = no rotation by line count
        std::uint64_t max_mb = 2048; // 0 = no rotation by size
    };

    struct RecordingConfig {
        std::filesystem::path output_dir; // injected by the host, never from the yaml
        RotationConfig rotation;
        GapPolicy on_gap = GapPolicy::kRecord;
        std::uint32_t flush_interval_mb = 64; // 0 = fdatasync only at segment boundaries
        WavelengthConfig wavelengths;
        double max_duration_s = 0.0; // 0 = unlimited
        std::uint64_t max_frames = 0; // 0 = unlimited
    };

    struct RawFeature {
        std::string name;
        std::string type; // int | float | bool | enum | string | command
        std::string value; // enum entry / string value
        std::int64_t int_value = 0;
        double float_value = 0.0;
        bool bool_value = false;
    };

    struct FeatureConfig {
        std::vector<RawFeature> raw; // applied in order, after the structured settings
    };

    struct LogConfig {
        double stats_interval_s = 2.5; // 0 = off
    };

    struct ReferenceConfig {
        double duration_s = 5.0; // per phase (white and dark), 0.1..3600 seconds
    };

    struct AppConfig {
        DeviceConfig device;
        NetworkConfig network;
        AcquisitionConfig acquisition;
        SensorTriggerConfig sensor_trigger;
        RecordingConfig recording; // yaml section `output`
        FeatureConfig features;
        LogConfig logging;
        ReferenceConfig reference; // used only by fx10_reference
    };

    // Throws ConfigError.
    AppConfig LoadAppConfig(const std::string &path);

    AppConfig LoadAppConfigText(const std::string &yaml_text);

    // "y1 h1;y2 h2;..." -> (start_row, height) list; throws ConfigError
    std::vector<std::pair<int, int> > ParseMroiRegions(const std::string &multiband_string);
} // namespace fx10
