#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app_config.h"


// The ordered camera writes that turn a freshly connected FX10e into the
// configured camera. Pure and SDK-free: CameraControl just walks the plan.
// Ordering (manual pages): binning before PixelFormat and everything else
// (p.46); MROI after binning, as an index register (p.26); ExposureMode before
// ExposureTime (p.29); TriggerSelector before TriggerMode; EnAcquisitionFrameRate
// written in both trigger branches (p.28); AcquisitionFrameRate clamped to the
// live node range (the FX10e rejects out-of-range writes); the missed-trigger
// counter after the trigger is armed (p.27); features.raw last.
namespace fx10 {
    // GenICam node names, verified with eBUS Player 6.5.1
    namespace node {
        inline constexpr const char *kAcquisitionMode = "AcquisitionMode";
        inline constexpr const char *kSpectralBinning = "BinningVertical";
        inline constexpr const char *kSpatialBinning = "BinningHorizontal";
        inline constexpr const char *kPixelFormat = "PixelFormat";
        inline constexpr const char *kExposureTime = "ExposureTime"; // us
        inline constexpr const char *kExposureMode = "ExposureMode"; // Timed | TriggerControlled
        inline constexpr const char *kFrameRate = "AcquisitionFrameRate";
        inline constexpr const char *kFrameRateEnable = "EnAcquisitionFrameRate";
        inline constexpr const char *kStatusLine = "EnStatusLine";
        inline constexpr const char *kTriggerSelector = "TriggerSelector";
        inline constexpr const char *kTriggerMode = "TriggerMode";
        inline constexpr const char *kTriggerActivation = "TriggerActivation";
        inline constexpr const char *kTriggerDelay = "TriggerDelay"; // us
        inline constexpr const char *kTriggerSource = "TriggerSource";
        inline constexpr const char *kMroiEnable = "MROI_Enable";
        inline constexpr const char *kMroiIndex = "MROI_Index";
        inline constexpr const char *kMroiY = "MROI_Y";
        inline constexpr const char *kMroiH = "MROI_H";
        // Counter1 is bound to MissedTrigger and reset by the driver (p.27)
        inline constexpr const char *kMissedTriggerCount = "Counter1_Value";
        inline constexpr const char *kMissedTriggerReset = "Counter1_Reset";
        inline constexpr const char *kMissedTriggerSource = "Counter1_EventSource";
        // ProcPCB 80 C | FPGA 90 C (p.44): the selector is written before every read
        inline constexpr const char *kDeviceTemperature = "DeviceTemperature";
        inline constexpr const char *kDeviceTemperatureSelector = "DeviceTemperatureSelector";
    } // namespace node

    enum class WriteKind { kInt, kFloat, kBool, kEnum, kString, kCommand };

    struct FeatureWrite {
        std::string node;
        WriteKind kind = WriteKind::kInt;
        std::int64_t int_value = 0;
        double float_value = 0.0;
        bool bool_value = false;
        std::string text; // enum entry / string value
        bool strict = true; // read-back mismatch (incl. a camera clamp) fails the bring-up; false = warn
        bool required = true; // false: a node this firmware lacks is a WARN, not a failure
        bool clamp_to_node_range = false; // clamp to the node's live [min, max] first (frame rate only)
    };

    std::vector<FeatureWrite> BuildApplyPlan(const AcquisitionConfig &acquisition, const std::vector<RawFeature> &raw);
} // namespace fx10
