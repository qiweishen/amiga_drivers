#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "driver_app.h"
#include "ebus_driver/raw_recorder.h"


class PvDeviceGEV;
class PvStreamGEV;

namespace common {
    struct Config;
    class SensorSyncHub;
}

namespace gox {
    struct AppConfig;
}

namespace fx10 {
    struct AppConfig;
}

namespace ebus_driver {
    // Identities use the SDK headers this camera engine was compiled against.
    // The RAW binary layout version is independent of this SDK provenance.
    const char *SdkHeaderVersion() noexcept;
    std::string DeviceMetadataFormat();
    std::string RawRecordingFormat();

    using StopPredicate = std::function<bool()>;

    struct HealthSample {
        nlohmann::json values = nlohmann::json::object();
        std::string failure;
        std::optional<std::uint64_t> triggered_frames;
    };

    struct DeviceHooks {
        std::function<void(PvDeviceGEV &, const StopPredicate &)> prepare;
        std::function<void(PvDeviceGEV &, const StopPredicate &)> configure;
        std::function<void(PvStreamGEV &)> configure_stream;
        // Perform start-time actions and return only their results for profile_start.actions.
        std::function<nlohmann::json(PvDeviceGEV &, const StopPredicate &)> before_start;
        // Called once during Init for profile metadata after configuration.
        // The engine supplies the SDK identity and recording format in device.json.
        std::function<nlohmann::json(PvDeviceGEV &)> metadata;
        std::function<HealthSample(PvDeviceGEV &)> sample;
    };

    struct CameraOptions {
        std::string id, kind, module;
        std::string mac, ip, local_ip;
        RecorderOptions recording;
        std::uint32_t packet_size = 0, socket_rx_buffer_mb = 32;
        std::uint32_t sdk_buffers = 16, queue_frames = 0, max_buffer_memory_mb = 512;
        std::uint32_t retrieve_timeout_ms = 1000;
        double expected_rate_hz = 1, stall_budget_s = 2, telemetry_interval_s = 5;
        double statistics_interval_s = 2.5; // zero disables display-rate messages
        double max_duration_s = 0;
        std::uint64_t max_frames = 0;
        bool expect_data = true;
        bool sensor_sync = false;
        int sensor_channel = 0;
        double pulse_hz = 0;
        // Extended IDs must be confirmed by the profile; false uses the 16-bit cycle.
        bool extended_block_ids = false;
    };

    struct CameraDefinition {
        CameraOptions options;
        DeviceHooks hooks;
    };

    class EbusCameraDriver final : public common::IDriverApp {
    public:
        EbusCameraDriver(CameraDefinition definition, std::shared_ptr<common::SensorSyncHub> sync = {});

        ~EbusCameraDriver() override;

        bool Init(const StopPredicate &external_stop = {}) override;

        void Run() override;

        void Shutdown() override;

        [[nodiscard]] std::optional<std::uint64_t> MicrosSinceLastData() const override;

        [[nodiscard]] nlohmann::ordered_json FinalStatistics() const override;

        [[nodiscard]] std::filesystem::path Directory() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // Existing YAML shapes and GUI identities remain supported
    std::vector<CameraDefinition> LoadGoxCameras(const std::filesystem::path &config,
                                                 const std::filesystem::path &output);

    CameraDefinition LoadFx10Camera(const std::filesystem::path &config,
                                    const std::filesystem::path &output);

    // Tools can apply their existing in-memory overrides without serializing a
    // second configuration. output is the GOX group directory / FX10 camera directory
    std::vector<CameraDefinition> MakeGoxCameras(const gox::AppConfig &config,
                                                 const std::filesystem::path &config_source,
                                                 const std::filesystem::path &output);

    CameraDefinition MakeFx10Camera(const fx10::AppConfig &config,
                                    const std::filesystem::path &config_source,
                                    const std::filesystem::path &output,
                                    bool dark_reference = false);
} // namespace ebus_driver
