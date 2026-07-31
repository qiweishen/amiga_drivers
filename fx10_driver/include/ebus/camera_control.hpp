#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app_config.hpp"


class PvDevice;
class PvGenParameterArray;


namespace fx10 {
    class ControlError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class CameraControl {
    public:
        // Does not own the device; `device` must outlive this object.
        CameraControl(PvDevice &device, const FeatureConfig &features);

        // --- direct node access (throws ControlError on missing/unusable node) ---
        std::int64_t getInt(const std::string &node);

        double getFloat(const std::string &node);

        bool getBool(const std::string &node);

        std::string getEnum(const std::string &node);

        std::string getString(const std::string &node);

        std::int64_t setInt(const std::string &node, std::int64_t value); // returns read-back
        double setFloat(const std::string &node, double value); // returns read-back
        void setBool(const std::string &node, bool value);

        void setEnum(const std::string &node, const std::string &entry);

        void setString(const std::string &node, const std::string &value);

        void execute(const std::string &node);

        // --- role-based access (false = role unmapped and skipped with a WARN) ---
        bool setIntByRole(const std::string &role, std::int64_t value);

        bool setFloatByRole(const std::string &role, double value);

        bool setBoolByRole(const std::string &role, bool value);

        bool setEnumByRole(const std::string &role, const std::string &entry);

        bool setStringByRole(const std::string &role, const std::string &value);

        // nullopt when the role is unmapped or the node is unreadable (logged DEBUG).
        // Safe to call while streaming (control channel reads only) — used for the
        // missed-trigger counter and device-temperature polling.
        std::optional<std::int64_t> tryGetIntByRole(const std::string &role);

        std::optional<double> tryGetFloatByRole(const std::string &role);

        // Apply the acquisition configuration in the documented order: pixel format ->
        // binning -> MROI -> status line -> exposure mode+time -> acquisition mode ->
        // frame rate (freerun) or the trigger group (external) -> raw passthrough
        // (features.raw IS applied here, last, so it can override anything)
        void applyAcquisitionConfig(const AcquisitionConfig &acquisition);

        void applyRawFeatures(const std::vector<RawFeature> &raw);

        struct Geometry {
            std::int64_t width = 0; // spatial samples
            // Spectral bands. Per the FX10 manual the status line OVERWRITES the last
            // band row when enabled (height unchanged)
            std::int64_t height = 0;
            std::uint32_t payload_size = 0;
            std::string pixel_format;
        };

        Geometry readGeometry();

        // One grep-friendly line per feature:
        // category | name | type | access | value | [min..max, inc] | {enum entries}
        // `array` defaults to the device node map; pass a stream's GetParameters() to
        // dump stream statistics instead. Returns the number of features dumped.
        std::size_t dumpAllFeatures(std::ostream &os, PvGenParameterArray *array = nullptr);

    private:
        PvDevice &device_;
        FeatureConfig features_;
    };
} // namespace fx10
