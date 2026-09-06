#include "apply_plan.h"

namespace fx10 {
    namespace {
        FeatureWrite Write(const char *node, WriteKind kind) {
            FeatureWrite w;
            w.node = node;
            w.kind = kind;
            return w;
        }

        void AddInt(std::vector<FeatureWrite> &plan, const char *node, std::int64_t value) {
            FeatureWrite w = Write(node, WriteKind::kInt);
            w.int_value = value;
            plan.push_back(std::move(w));
        }

        void AddFloat(std::vector<FeatureWrite> &plan, const char *node, double value, bool clamp = false) {
            FeatureWrite w = Write(node, WriteKind::kFloat);
            w.float_value = value;
            w.clamp_to_node_range = clamp;
            plan.push_back(std::move(w));
        }

        void AddBool(std::vector<FeatureWrite> &plan, const char *node, bool value) {
            FeatureWrite w = Write(node, WriteKind::kBool);
            w.bool_value = value;
            plan.push_back(std::move(w));
        }

        void AddEnum(std::vector<FeatureWrite> &plan, const char *node, std::string entry, bool required = true) {
            FeatureWrite w = Write(node, WriteKind::kEnum);
            w.text = std::move(entry);
            w.required = required;
            plan.push_back(std::move(w));
        }

        void AddCommand(std::vector<FeatureWrite> &plan, const char *node, bool required = true) {
            FeatureWrite w = Write(node, WriteKind::kCommand);
            w.required = required;
            plan.push_back(std::move(w));
        }
    } // namespace


    std::vector<FeatureWrite> BuildApplyPlan(const AcquisitionConfig &acq, const std::vector<RawFeature> &raw) {
        std::vector<FeatureWrite> plan;

        // 1. Geometry: binning first (p.46), then pixel format
        AddInt(plan, node::kSpatialBinning, acq.spatial_binning);
        AddInt(plan, node::kSpectralBinning, acq.spectral_binning);
        AddEnum(plan, node::kPixelFormat, acq.pixel_format);

        // 2. MROI as an index register (p.26); "off" is asserted, it survives a power cycle
        if (acq.mroi.enabled) {
            const auto regions = ParseMroiRegions(acq.mroi.multiband_string);
            for (std::size_t i = 0; i < regions.size(); ++i) {
                AddInt(plan, node::kMroiIndex, static_cast<std::int64_t>(i));
                AddInt(plan, node::kMroiY, regions[i].first);
                AddInt(plan, node::kMroiH, regions[i].second);
            }
            AddBool(plan, node::kMroiEnable, true);
        } else {
            AddBool(plan, node::kMroiEnable, false);
        }

        // 3. Status line replaces the last image row (p.34)
        AddBool(plan, node::kStatusLine, acq.status_line);

        // 4. ExposureMode before ExposureTime: TriggerControlled makes ExposureTime unavailable (p.29)
        const bool pulse_width = acq.trigger.mode == TriggerMode::kExternal &&
                                 acq.trigger.exposure_control == ExposureControl::kPulseWidth;
        AddEnum(plan, node::kExposureMode, pulse_width ? "TriggerControlled" : "Timed");
        if (!pulse_width) {
            AddFloat(plan, node::kExposureTime, acq.exposure_ms * 1000.0);
        }

        AddEnum(plan, node::kAcquisitionMode, "Continuous");

        // 5. Trigger group; EnAcquisitionFrameRate in both branches (p.28)
        if (acq.trigger.mode == TriggerMode::kFreerun) {
            AddEnum(plan, node::kTriggerSelector, acq.trigger.selector_entry);
            AddEnum(plan, node::kTriggerMode, "Off");
            AddBool(plan, node::kFrameRateEnable, true);
            AddFloat(plan, node::kFrameRate, acq.frame_rate_hz, /*clamp=*/true);
        } else {
            AddEnum(plan, node::kTriggerSelector, acq.trigger.selector_entry);
            AddEnum(plan, node::kTriggerSource, acq.trigger.source_entry);
            AddEnum(plan, node::kTriggerActivation,
                    acq.trigger.activation == TriggerActivation::kRising ? "RisingEdge" : "FallingEdge");
            AddFloat(plan, node::kTriggerDelay, acq.trigger.delay_ms * 1000.0);
            AddEnum(plan, node::kTriggerMode, "On");
            AddBool(plan, node::kFrameRateEnable, false);
            // 5b. Missed-trigger counter (p.27); optional: a firmware without it costs the metric only
            AddEnum(plan, node::kMissedTriggerSource, "MissedTrigger", /*required=*/false);
            AddCommand(plan, node::kMissedTriggerReset, /*required=*/false);
        }

        // 6. Operator escape hatch, last; a clamp only warns (not driver-derived intent)
        for (const RawFeature &f: raw) {
            FeatureWrite w;
            w.node = f.name;
            w.strict = false;
            if (f.type == "int") {
                w.kind = WriteKind::kInt;
                w.int_value = f.int_value;
            } else if (f.type == "float") {
                w.kind = WriteKind::kFloat;
                w.float_value = f.float_value;
            } else if (f.type == "bool") {
                w.kind = WriteKind::kBool;
                w.bool_value = f.bool_value;
            } else if (f.type == "command") {
                w.kind = WriteKind::kCommand;
            } else if (f.type == "string") {
                w.kind = WriteKind::kString;
                w.text = f.value;
            } else {
                w.kind = WriteKind::kEnum;
                w.text = f.value;
            }
            plan.push_back(std::move(w));
        }
        return plan;
    }
} // namespace fx10
