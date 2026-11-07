#pragma once

#include "../include/app_config.h"
#include "../include/apply_plan.h"

#include <utility>

namespace fx10::tool {
    // GUI snapshots and white/dark references share the same timing policy.
    // Keep exposure, rate, geometry and raw processing settings from the source
    // YAML; only the trigger/timing controls are forced for standalone capture.
    inline void ForceFreerun(AppConfig &config) {
        config.acquisition.trigger.mode = TriggerMode::kFreerun;
        config.acquisition.trigger.selector_entry = "FrameStart";
        config.acquisition.trigger.exposure_control = ExposureControl::kCamera;
        config.sensor_trigger.enabled = false;

        const auto add_enum = [&](const char *name, const char *value) {
            RawFeature write;
            write.name = name;
            write.type = "enum";
            write.value = value;
            config.features.raw.push_back(std::move(write));
        };
        // These final writes run after all operator overrides. The existing
        // CameraControl path verifies bool/enum equality and audits each write.
        add_enum(node::kTriggerMode, "Off");
        add_enum(node::kTriggerSelector, "FrameStart");
        add_enum(node::kTriggerMode, "Off");
        add_enum(node::kExposureMode, "Timed");
        add_enum(node::kAcquisitionMode, "Continuous");
        RawFeature rate_enable;
        rate_enable.name = node::kFrameRateEnable;
        rate_enable.type = "bool";
        rate_enable.bool_value = true;
        config.features.raw.push_back(std::move(rate_enable));
    }
} // namespace fx10::tool
