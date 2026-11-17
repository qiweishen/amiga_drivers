#pragma once

#include <optional>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app_config.h"
#include "apply_plan.h"
#include "ebus_config_file.h"
#include <nlohmann/json.hpp>


class PvDevice;
class PvGenParameterArray;


namespace fx10 {
    // Borrowed device, no stream open. Refreshes the node cache and checks
    // readback after each command, without reconnecting. False means cancellation.
    bool PrepareFactoryDefaults(PvDevice &device, const std::function<bool()> &stop = {});
    class ControlError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class CameraControl {
    public:
        // Does not own the device; `device` must outlive this object.
        explicit CameraControl(PvDevice &device);

        // --- direct node access (throws ControlError on missing/unusable node) ---
        [[nodiscard]] std::int64_t GetInt(const std::string &node) const;

        [[nodiscard]] double GetFloat(const std::string &node) const;

        [[nodiscard]] bool GetBool(const std::string &node) const;

        [[nodiscard]] std::string GetEnum(const std::string &node) const;

        [[nodiscard]] std::string GetString(const std::string &node) const;

        [[nodiscard]] std::int64_t SetInt(const std::string &node, std::int64_t value) const; // returns read-back

        double SetFloat(const std::string &node, double value); // returns read-back

        void SetBool(const std::string &node, bool value);

        void SetEnum(const std::string &node, const std::string &entry);

        void SetString(const std::string &node, const std::string &value);

        void Execute(const std::string &node);

        // Execute exactly once and check the result. Caller must establish readiness on
        // the current connection before executing another command.
        void ExecuteFactoryCommand(const char *name, const std::function<bool()> &stop = {}) const;

        void VerifyControlReady() const;

        // After PrepareFactoryDefaults, before the stream open and ApplyAcquisitionConfig.
        // Requires DeviceSerialNumber == the file's directory name and the checksums.txt
        // SHA-256 and reads the file's Width/Height/OffsetX/OffsetY/Binning* values,
        // then loads the whole file with PvConfigurationReader::Load and applies it,
        // unfiltered, with Restore(0, device) TWICE (the file lists Height before
        // BinningVertical; the second pass starts from the file's binning). Control
        // readiness is re-verified after each pass, and the final geometry readback
        // must equal the file's values. Throws ControlError / ConfigError; nothing is
        // restored when a pre-load check fails. Result: EbusConfigRestore().
        void RestoreEbusConfiguration(const std::filesystem::path &pvcfg, const std::function<bool()> &stop = {});

        [[nodiscard]] const nlohmann::json &EbusConfigRestore() const noexcept { return ebus_config_; }

        // After all config writes, before StreamEnable/AcquisitionStart. Every
        // call writes MotorShutter_PulseRev = 1, reads it back from the device
        // (cache invalidated), then writes 200 and reads it back; its stored value
        // is NOT shutter position. Checks SDK write acknowledgement and value
        // readback, not motion completion. Throws on missing/unwritable/wrong-type
        // node, invalid range, SDK write/read failure, readback mismatch or
        // cancellation. No automatic retry or conditional skip based on readback.
        void OpenShutter(const std::function<bool()> &stop = {});
        // Reference capture only: same pulse/readback semantics, MotorShutter_PulseFwd = 1, then 204.
        void CloseShutter(const std::function<bool()> &stop = {});
        // Cached outcome of the latest pulse; no SDK reads and no state verification.
        [[nodiscard]] const nlohmann::json &ShutterPreparation() const noexcept { return shutter_preparation_; }

        // Best-effort telemetry reads, safe while Streaming (control channel only); nullopt when
        // unreadable (logged DEBUG). The temperature read writes DeviceTemperatureSelector first:
        // the two sensors have different limits (manual p.44).
        std::optional<double> TryReadTemperature(const std::string &selector_entry);

        [[nodiscard]] std::optional<std::int64_t> TryGetInt(const std::string &node) const;

        [[nodiscard]] std::optional<double> TryGetFloat(const std::string &node) const;

        // Walks BuildApplyPlan() after loadFactoryDefaults and the stream open. Throws ControlError
        // on the first required write that cannot be applied or verified. After a
        // .pvcfg restore, the plan may change binning but not the detector window:
        // OffsetX/OffsetY, Width x BinningHorizontal and Height x BinningVertical must
        // equal the restored values, or this throws.
        void ApplyAcquisitionConfig(const AcquisitionConfig &acquisition, const std::vector<RawFeature> &raw);

        // One plan entry. Returns false when a non-required write was skipped (node absent).
        bool ApplyWrite(const FeatureWrite &write);

        // Clamp to a float node's live [min, max] (the FX10e rejects an
        // out-of-range write instead of clamping). Returns `value` unchanged
        // when the node or its bounds are unreadable.
        double ClampToNodeRange(const std::string &node, double value);

        struct Geometry {
            std::int64_t width = 0; // spatial samples
            // Spectral bands. Per the FX10 manual the status line OVERWRITES the last
            // band row when enabled (height unchanged)
            std::int64_t height = 0;
            std::int64_t offset_x = -1; // -1: node unavailable, cannot verify calibration
            std::int64_t offset_y = -1;
            std::uint32_t payload_size = 0;
            std::string pixel_format;
            bool status_line = false; // EnStatusLine readback: on = last band row is camera status (p.34)
        };

        Geometry ReadGeometry();
        // Static configuration snapshot; unknown/unreadable nodes are explicit.
        // Start-time shutter results are returned separately by ShutterPreparation().
        // Collect only while acquisition is idle. Selector-scoped apply entries
        // retain their immediate readback in plan order.
        nlohmann::json CollectDeviceMetadata() const;
        nlohmann::json ReadNodeMetadata(const std::string &name) const;
    private:
        void WriteShutterPulse(bool open, const std::function<bool()> &stop);
        // Fresh (cache-invalidated) Width/Height/OffsetX/OffsetY/Binning* readback.
        SensorGeometry ReadSensorGeometry() const;
        PvDevice &device_;
        nlohmann::json applied_ = nlohmann::json::array();
        nlohmann::json shutter_preparation_ = {{"status", "not-attempted"}};
        nlohmann::json ebus_config_ = {{"status", "not-configured"}};
        std::optional<SensorGeometry> restored_geometry_; // set once the .pvcfg geometry check passes
        nlohmann::json sensor_window_ = {{"status", "not-checked"}};
    };
} // namespace fx10
