#pragma once

#include <optional>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app_config.h"
#include "apply_plan.h"


class PvDevice;
class PvGenParameterArray;


namespace fx10 {
    class StreamReceiver;

    // Connected receiver, no stream open. Refreshes the node cache and checks
    // readback after each command, without reconnecting. False means cancellation.
    bool PrepareFactoryDefaults(StreamReceiver &receiver, const std::function<bool()> &stop = {});
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

        // Best-effort telemetry reads, safe while Streaming (control channel only); nullopt when
        // unreadable (logged DEBUG). The temperature read writes DeviceTemperatureSelector first:
        // the two sensors have different limits (manual p.44).
        std::optional<double> TryReadTemperature(const std::string &selector_entry);

        [[nodiscard]] std::optional<std::int64_t> TryGetInt(const std::string &node) const;

        [[nodiscard]] std::optional<double> TryGetFloat(const std::string &node) const;

        // Walks BuildApplyPlan() after loadFactoryDefaults and the stream open. Throws ControlError
        // on the first required write that cannot be applied or verified.
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
        };

        Geometry ReadGeometry();
    private:
        PvDevice &device_;
    };
} // namespace fx10
