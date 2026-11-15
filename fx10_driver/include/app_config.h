#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "config_util.h"


// YAML-backed configuration (config/config-fx10.yaml). Strict schema: unknown
// keys are errors, omitted keys keep the defaults below.
namespace fx10 {
    using ConfigError = common::ConfigError;

    // Timing limits of the FX10e, set by the operator (2026-11-08); every trigger mode.
    // AcquisitionFrameRate ceiling: 163.02 Hz at BinningHorizontal = BinningVertical = 1,
    // 324.373 Hz for every other binning combination. ExposureTime maximum 419000 us.
    inline constexpr double kMaxFrameRateHzUnbinned = 163.02;
    inline constexpr double kMaxFrameRateHzBinned = 324.373;
    inline constexpr double kMaxExposureMs = 419.0;
    // The exposure may use at most 90 % of the frame (or pulse) period.
    inline constexpr double kExposurePeriodFraction = 0.9;

    inline double MaxFrameRateHz(int spatial_binning, int spectral_binning) {
        return spatial_binning == 1 && spectral_binning == 1 ? kMaxFrameRateHzUnbinned : kMaxFrameRateHzBinned;
    }

    // Longest exposure that fits a frame rate, ms: 0.9 * 1000 / frame_rate_hz
    inline double ExposureLimitMs(double frame_rate_hz) {
        return kExposurePeriodFraction * 1000.0 / frame_rate_hz;
    }

    // Inclusive check with a relative 1e-9 allowance, so an exposure computed as
    // exactly 90 % of a period is not rejected by floating-point rounding.
    inline bool ExposureFitsPeriod(double exposure_ms, double frame_rate_hz) {
        return exposure_ms <= ExposureLimitMs(frame_rate_hz) * (1.0 + 1e-9);
    }

    struct DeviceConfig {
        std::string mac; // discovery match; wins over ip
        std::string ip; // direct connect; used when mac is empty
        // eBUS Player .pvcfg restored after factory preparation; empty = not loaded.
        // Relative paths resolve against the project root; see ebus_config_file.h.
        std::string ebus_config;
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

    // The board's serial port is the rig's (config-main.yaml "Sensor Trigger: Port")
    struct SensorTriggerConfig {
        bool enabled = false;
        int trigger_channel = 0; // physical log ID; pairs 0/1 = FX, 2/3 = JAI (one rate per pair)
    };

    struct AcquisitionConfig {
        int spatial_binning = 1; // 1|2|4|8; actual width comes from device readback
        int spectral_binning = 2; // 1|2|4|8; actual height comes from device readback, not the optical-band estimate
        std::string pixel_format = "Mono12Packed";
        double exposure_ms = 5.0;
        double frame_rate_hz = 50.0; // freerun rate; expected pulse rate under external trigger
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
        std::vector<double> fwhm_list; // empty = FWHM metadata unavailable
    };

    struct RotationConfig {
        std::uint64_t max_lines = 0; // RAW SDK records; 0 = no count-based rotation
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

    // Exposure <= kMaxExposureMs, frame rate <= MaxFrameRateHz(binning), exposure <= 90 % of
    // the period. Run by the loader and again by tools after CLI overrides. Throws ConfigError.
    void CheckAcquisitionTiming(const AcquisitionConfig &acq);
} // namespace fx10
