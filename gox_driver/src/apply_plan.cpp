#include "apply_plan.h"

#include <cstdio>

namespace gox {
    namespace {
        // Same rendering as the old CameraController::FmtDouble, so log lines and
        // the values the camera receives are unchanged.
        std::string FmtDouble(double v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.10g", v);
            return buf;
        }


        void add(std::vector<FeatureWrite> &out, std::string name, std::string value, bool is_string,
                 bool strict = true, bool required = true) {
            FeatureWrite w;
            w.name = std::move(name);
            w.value = std::move(value);
            w.value_is_string = is_string;
            w.strict = strict;
            w.required = required;
            out.push_back(std::move(w));
        }


        // Trailing digits of a pixel format name that are not part of a "Packed"
        // suffix: BayerRG12Packed -> 12, Mono8 -> 8, RGB10V1Packed -> 10.
        int TrailingNumber(const std::string &s) {
            size_t end = s.size();
            // Skip a "Packed" suffix and anything else non-numeric at the end.
            while (end > 0 && (s[end - 1] < '0' || s[end - 1] > '9')) {
                --end;
            }
            size_t begin = end;
            while (begin > 0 && s[begin - 1] >= '0' && s[begin - 1] <= '9') {
                --begin;
            }
            // A pixel format's depth is one or two digits; anything longer is
            // not a depth, and std::stoi would be free to throw on it.
            if (begin == end || end - begin > 2) {
                return 0;
            }
            return std::stoi(s.substr(begin, end - begin));
        }
    } // namespace


    int PixelFormatBitDepth(const std::string &pixel_format) {
        const int n = TrailingNumber(pixel_format);
        // RGB10V1Packed's trailing "1" must not read as 1 bit: only the depths the
        // GO-X actually offers are accepted (manual p.137).
        if (n == 8 || n == 10 || n == 12) {
            return n;
        }
        if (pixel_format.rfind("RGB10", 0) == 0) {
            return 10;
        }
        if (pixel_format.rfind("RGB8", 0) == 0) {
            return 8;
        }
        return 0;
    }


    std::vector<FeatureWrite> BuildApplyPlan(const CameraConfig &cfg) {
        std::vector<FeatureWrite> plan;
        const AcquisitionConfig &acq = cfg.acquisition;

        // 1. 64-bit BlockIDs. The factory value is Off (manual p.129), so this
        // fires on every start; 16-bit IDs wrap every 65536 frames and would break
        // the gap accounting. It is also an input to the frame rate's and
        // GevSCPD's maxima, so it goes first.
        add(plan, "GevGVSPExtendedIDMode", "On", true);

        // 2. Sensor A/D depth. Leaving it at the factory 10 bits under a 12-bit
        // pixel format records 10 bits of data in 12-bit containers ("the image
        // may have gaps in the histogram", manual p.45/p.168). Never lowered:
        // 8 Bits also changes the sensitivity (p.45).
        if (acq.pixel_format) {
            if (PixelFormatBitDepth(*acq.pixel_format) == 12) {
                add(plan, "SensorDigitizationBits", "12", false);
            }
        }

        // 3. ROI, in the safe order: size first, then the offsets (the offsets'
        // maxima depend on the size; manual p.133-134). The factory offsets are
        // already 0, so they are only written when the config asks for a window.
        if (acq.roi) {
            const RoiConfig &roi = *acq.roi;
            if (roi.width != 0) {
                add(plan, "Width", std::to_string(roi.width), false);
            }
            if (roi.height != 0) {
                add(plan, "Height", std::to_string(roi.height), false);
            }
            if (roi.offset_x != 0) {
                add(plan, "OffsetX", std::to_string(roi.offset_x), false);
            }
            if (roi.offset_y != 0) {
                add(plan, "OffsetY", std::to_string(roi.offset_y), false);
            }
        }

        // 4. PixelFormat (factory BayerRG8, manual p.137).
        if (acq.pixel_format) {
            add(plan, "PixelFormat", *acq.pixel_format, true);
        }

        // 4b. Blemish correction: factory Enable (p.156); "Disable all" also drops the factory
        // black-blemish interpolation (p.82). Written only when off, after PixelFormat
        if (!acq.blemish_correction) {
            add(plan, "BlemishEnable", "0", false);
        }

        // 5. Bandwidth share before the frame rate: the margin is one of the
        // inputs to AcquisitionFrameRate's maximum (manual p.128, p.141).
        if (cfg.network.throughput_safety_margin_pct != 0) {
            add(plan, "NetworkThroughputSafetyMargin",
                std::to_string(cfg.network.throughput_safety_margin_pct), false);
        }

        // 6. Frame rate before exposure. Under an external trigger the pulse train
        // drives frame timing and AcquisitionFrameRate is not writable (manual p.36).
        const bool external = acq.trigger.mode == TriggerMode::kExternal;
        if (acq.frame_rate_hz && !external) {
            add(plan, "AcquisitionFrameRate", FmtDouble(*acq.frame_rate_hz), false);
        }

        // 7. Exposure. The config is in milliseconds (fx10 convention), the
        // GenICam node in microseconds.
        if (acq.exposure_ms) {
            add(plan, "ExposureTime", FmtDouble(*acq.exposure_ms * 1000.0), false);
        }

        // 8. Gain. GainSelector is AnalogAll out of the factory load and the GO-X
        // has no "All" entry (manual p.148), so the selector is not written.
        if (acq.gain) {
            add(plan, "Gain", FmtDouble(*acq.gain), false);
        }

        // 9. Trigger. Selector first so TriggerMode targets the right trigger;
        // freerun needs nothing at all (the factory TriggerMode is Off, p.141).
        if (external) {
            add(plan, "TriggerSelector", acq.trigger.selector_entry, true);
            const bool numeric_source =
                    !acq.trigger.source_entry.empty() &&
                    acq.trigger.source_entry.find_first_not_of("0123456789") == std::string::npos;
            add(plan, "TriggerSource", acq.trigger.source_entry, !numeric_source);
            add(plan, "TriggerActivation",
                acq.trigger.activation == TriggerActivation::kRising ? "RisingEdge" : "FallingEdge",
                true);
            add(plan, "TriggerMode", "On", true);

            // 9b. Counter0 counts received FrameTrigger events (p.115-116, p.160): triggers the camera
            // swallowed are otherwise invisible. required=false: a camera without it costs only trig=
            add(plan, "CounterSelector", "0", false, /*strict=*/true, /*required=*/false);
            add(plan, "CounterEventSource", "1", false, /*strict=*/true, /*required=*/false);
        }

        // 10. Operator escape hatch, last so it can override anything above.
        // Clamping only warns here (fx10 features.raw semantics).
        for (const RawFeature &f: cfg.features.raw) {
            add(plan, f.name, f.value, f.value_is_string, /*strict=*/false);
        }

        return plan;
    }
} // namespace gox
