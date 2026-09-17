#pragma once

// The ordered GenICam writes that turn a factory-loaded GO-X into the configured camera.
// Pure and SDK-free: CameraController just walks the plan. Ordering (manual pages):
// GevGVSPExtendedIDMode first (p.130, p.141); SensorDigitizationBits before PixelFormat (p.45,
// p.168); ROI before PixelFormat, offsets after size (p.133-134); NetworkThroughputSafetyMargin
// before the frame rate (p.128); AcquisitionFrameRate before ExposureTime (p.142);
// TriggerSelector before TriggerMode (p.141); BlemishEnable after PixelFormat (p.156);
// the Counter0 binding after the trigger (p.115-116, p.160); the Line2 ExposureActive strobe
// (LineSelector=21 then LineSource=4, p.143-144) after the trigger group; features.raw last.

#include <string>
#include <vector>

#include "app_config.h"

namespace gox {
    struct FeatureWrite {
        std::string name;
        std::string value;
        bool value_is_string = false; // true = enum entry / string; false = typed by the camera node
        bool strict = true; // a read-back mismatch (incl. a camera clamp) fails the bring-up; false = warn
        bool required = true; // false: a missing feature is skipped, a refused value warns (Counter0 binding)
    };

    // A/D depth implied by a pixel format name ("BayerRG12Packed" -> 12), 0 = unknown
    int PixelFormatBitDepth(const std::string &pixel_format);

    // Never throws; an absent optional leaves the feature at its factory value
    std::vector<FeatureWrite> BuildApplyPlan(const CameraConfig &cfg);
} // namespace gox
