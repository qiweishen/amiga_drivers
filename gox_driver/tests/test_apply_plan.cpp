// The ordering contract of the GenICam write plan (src/apply_plan.cpp). Every
// constraint asserted here has a manual page behind it; see apply_plan.hpp.

#include "apply_plan.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {
    gox::CameraConfig BaseCamera() {
        gox::CameraConfig c;
        c.id = "cam0";
        c.device.ip = "10.0.0.2";
        return c;
    }

    std::vector<std::string> Names(const std::vector<gox::FeatureWrite> &plan) {
        std::vector<std::string> out;
        out.reserve(plan.size());
        for (const gox::FeatureWrite &f: plan) {
            out.push_back(f.name);
        }
        return out;
    }

    // Index of a feature in the plan, or -1.
    int IndexOf(const std::vector<gox::FeatureWrite> &plan, const std::string &name) {
        for (size_t i = 0; i < plan.size(); ++i) {
            if (plan[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    const gox::FeatureWrite *Find(const std::vector<gox::FeatureWrite> &plan, const std::string &name) {
        const int i = IndexOf(plan, name);
        return i < 0 ? nullptr : &plan[static_cast<size_t>(i)];
    }
} // namespace

TEST_CASE("apply_plan: a bare camera forces the 64-bit block IDs, the raw blemish state and the strobe") {
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(BaseCamera());
    REQUIRE(plan.size() == 5u);
    CHECK(plan[0].name == "GevGVSPExtendedIDMode");
    CHECK(plan[0].value == "On");
    CHECK(plan[0].value_is_string);
    CHECK(plan[0].strict);
    CHECK(plan[0].required);
    // blemish_correction defaults to false (the rawest state), and false is not
    // the factory value, so even a config that asks for nothing else writes it.
    CHECK(plan[1].name == "BlemishEnable");
    CHECK(plan[1].value == "0");
    // The Line2 ExposureActive strobe is the rig's default too: the factory
    // LineSource for Line2 copies the Line5 input, useless for timing.
    CHECK(plan[2].name == "LineSelector");
    CHECK(plan[3].name == "LineSource");
    CHECK(plan[4].name == "LineInverter");
    // freerun needs no trigger write at all: the factory TriggerMode is Off.
    CHECK(IndexOf(plan, "TriggerMode") == -1);
}

TEST_CASE("apply_plan: the ExposureActive strobe leaves on Line2 Opt Out, in freerun and under a trigger") {
    gox::CameraConfig c = BaseCamera();
    for (const gox::TriggerMode mode: {gox::TriggerMode::kFreerun, gox::TriggerMode::kExternal}) {
        c.acquisition.trigger.mode = mode;
        c.features.raw = {gox::RawFeature{"BlackLevel", "0", false}};
        const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
        const int selector = IndexOf(plan, "LineSelector");
        const int source = IndexOf(plan, "LineSource");
        const int inverter = IndexOf(plan, "LineInverter");
        REQUIRE(selector >= 0);
        // The selector points at Line2 before the source is written and read back
        CHECK(selector < source);
        CHECK(source < inverter);
        CHECK(inverter < IndexOf(plan, "BlackLevel")); // still ahead of the operator's escape hatch
        CHECK(Find(plan, "LineSelector")->value == "21"); // Line2 Opt Out1 (p.143)
        CHECK(Find(plan, "LineSource")->value == "4"); // ExposureActive (p.144)
        CHECK(Find(plan, "LineInverter")->value == "0");
        for (const char *n: {"LineSelector", "LineSource", "LineInverter"}) {
            CAPTURE(n);
            CHECK_FALSE(Find(plan, n)->value_is_string); // the manual's integer values
            CHECK(Find(plan, n)->strict);
            CHECK(Find(plan, n)->required);
        }
        if (mode == gox::TriggerMode::kExternal) {
            CHECK(IndexOf(plan, "TriggerMode") < selector); // after the trigger group
        }
    }
    c.acquisition.trigger.exposure_active_output = false;
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
    CHECK(IndexOf(plan, "LineSelector") == -1);
    CHECK(IndexOf(plan, "LineSource") == -1);
    CHECK(IndexOf(plan, "LineInverter") == -1);
}

TEST_CASE("apply_plan: trigger delay and input filter are written only when they leave the factory value") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.trigger.mode = gox::TriggerMode::kExternal;
    CHECK(IndexOf(gox::BuildApplyPlan(c), "TriggerDelay") == -1);
    CHECK(IndexOf(gox::BuildApplyPlan(c), "OptInFilter") == -1);
    c.acquisition.trigger.delay_ms = 1.5;
    c.acquisition.trigger.input_filter_ns = 500;
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
    CHECK(Find(plan, "TriggerDelay")->value == "1500"); // microseconds (p.142)
    CHECK(Find(plan, "OptInFilter")->value == "500"); // nanoseconds (p.145)
    // Both before the trigger is armed
    CHECK(IndexOf(plan, "TriggerDelay") < IndexOf(plan, "TriggerMode"));
    CHECK(IndexOf(plan, "OptInFilter") < IndexOf(plan, "TriggerMode"));
    // Freerun never touches them
    c.acquisition.trigger.mode = gox::TriggerMode::kFreerun;
    CHECK(IndexOf(gox::BuildApplyPlan(c), "TriggerDelay") == -1);
    CHECK(IndexOf(gox::BuildApplyPlan(c), "OptInFilter") == -1);
}

TEST_CASE("apply_plan: blemish correction is only written when it differs from the factory value") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.pixel_format = "BayerRG12Packed";
    c.network.throughput_safety_margin_pct = 46;

    SUBCASE("false disables it, as an integer enum value, strictly verified") {
        c.acquisition.blemish_correction = false;
        const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
        const gox::FeatureWrite *w = Find(plan, "BlemishEnable");
        REQUIRE(w != nullptr);
        CHECK(w->value == "0");
        CHECK_FALSE(w->value_is_string);
        CHECK(w->strict);
        CHECK(w->required);
        // After the Bayer phase and the readout window, before the bandwidth
        // margin that starts the timing chain.
        CHECK(IndexOf(plan, "PixelFormat") < IndexOf(plan, "BlemishEnable"));
        CHECK(IndexOf(plan, "BlemishEnable") < IndexOf(plan, "NetworkThroughputSafetyMargin"));
    }

    SUBCASE("true is the factory value, so UserSetLoad already established it") {
        c.acquisition.blemish_correction = true;
        const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
        CHECK(IndexOf(plan, "BlemishEnable") == -1);
    }
}

TEST_CASE("apply_plan: an external trigger binds Counter0 to the received trigger events") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.trigger.mode = gox::TriggerMode::kExternal;
    c.features.raw.push_back(gox::RawFeature{"BlackLevel", "0", false});
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);

    const gox::FeatureWrite *selector = Find(plan, "CounterSelector");
    const gox::FeatureWrite *source = Find(plan, "CounterEventSource");
    REQUIRE(selector != nullptr);
    REQUIRE(source != nullptr);
    CHECK(selector->value == "0"); // Counter0
    CHECK(source->value == "1"); // FrameTrigger
    CHECK_FALSE(selector->value_is_string);
    CHECK_FALSE(source->value_is_string);
    // A camera without counters must not cost a recording.
    CHECK_FALSE(selector->required);
    CHECK_FALSE(source->required);
    // Armed after the trigger, still ahead of the operator's escape hatch.
    CHECK(IndexOf(plan, "TriggerMode") < IndexOf(plan, "CounterSelector"));
    CHECK(IndexOf(plan, "CounterSelector") < IndexOf(plan, "CounterEventSource"));
    CHECK(IndexOf(plan, "CounterEventSource") < IndexOf(plan, "BlackLevel"));
    // CounterReset is a runtime command tied to AcquisitionStart, not a
    // configuration write: it must not be in the plan.
    CHECK(IndexOf(plan, "CounterReset") == -1);

    // Nothing else is optional.
    for (const gox::FeatureWrite &f: plan) {
        if (f.name != "CounterSelector" && f.name != "CounterEventSource") {
            CHECK(f.required);
        }
    }
}

TEST_CASE("apply_plan: freerun has no trigger events to count") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.frame_rate_hz = 3.0;
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
    CHECK(IndexOf(plan, "CounterSelector") == -1);
    CHECK(IndexOf(plan, "CounterEventSource") == -1);
}

TEST_CASE("apply_plan: GevGVSPExtendedIDMode is always written first") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.pixel_format = "BayerRG12Packed";
    c.acquisition.frame_rate_hz = 3.0;
    c.acquisition.exposure_ms = 150.0;
    c.acquisition.gain = 2.0;
    c.network.throughput_safety_margin_pct = 46;
    c.features.raw.push_back(gox::RawFeature{"BlackLevel", "0", false});
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
    REQUIRE(!plan.empty());
    CHECK(plan.front().name == "GevGVSPExtendedIDMode");
    // ... and the escape hatch is always last, so it can override everything.
    CHECK(plan.back().name == "BlackLevel");
}

TEST_CASE("apply_plan: SensorDigitizationBits follows the pixel format's depth") {
    struct Case {
        const char *format;
        bool expect_write;
    };
    const Case cases[] = {
        {"BayerRG12Packed", true}, {"BayerRG12", true}, {"Mono12", true}, {"Mono12Packed", true},
        {"BayerRG10Packed", false}, {"BayerRG10", false}, {"Mono10", false},
        {"BayerRG8", false}, {"Mono8", false}, {"RGB8", false}, {"RGB10V1Packed", false},
    };
    for (const Case &c: cases) {
        CAPTURE(c.format);
        gox::CameraConfig cfg = BaseCamera();
        cfg.acquisition.pixel_format = c.format;
        const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(cfg);
        const gox::FeatureWrite *w = Find(plan, "SensorDigitizationBits");
        if (c.expect_write) {
            REQUIRE(w != nullptr);
            CHECK(w->value == "12");
            CHECK_FALSE(w->value_is_string); // integer enum value, not a display name
            CHECK(w->strict);
            // It must precede PixelFormat: the A/D depth changes the format's limits.
            CHECK(IndexOf(plan, "SensorDigitizationBits") < IndexOf(plan, "PixelFormat"));
        } else {
            // Never lowered below the factory 10 bits.
            CHECK(w == nullptr);
        }
    }

    // No pixel format configured: nothing to derive from, nothing written.
    const std::vector<gox::FeatureWrite> bare = gox::BuildApplyPlan(BaseCamera());
    CHECK(Find(bare, "SensorDigitizationBits") == nullptr);
    CHECK(Find(bare, "PixelFormat") == nullptr);
}

TEST_CASE("apply_plan: pixel_format_bit_depth") {
    CHECK(gox::PixelFormatBitDepth("BayerRG12Packed") == 12);
    CHECK(gox::PixelFormatBitDepth("BayerGB10Packed") == 10);
    CHECK(gox::PixelFormatBitDepth("Mono8") == 8);
    CHECK(gox::PixelFormatBitDepth("RGB10p32") == 10); // trailing 32 is not a depth
    CHECK(gox::PixelFormatBitDepth("RGB10V1Packed") == 10); // trailing 1 is not a depth
    CHECK(gox::PixelFormatBitDepth("RGB8") == 8);
    CHECK(gox::PixelFormatBitDepth("Nonsense") == 0);
    CHECK(gox::PixelFormatBitDepth("") == 0);
}

TEST_CASE("apply_plan: freerun writes the frame rate before the exposure") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.frame_rate_hz = 3.0;
    c.acquisition.exposure_ms = 150.0;
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);

    const int rate = IndexOf(plan, "AcquisitionFrameRate");
    const int exposure = IndexOf(plan, "ExposureTime");
    REQUIRE(rate >= 0);
    REQUIRE(exposure >= 0);
    // The exposure ceiling is the frame period: writing the exposure first gets
    // it clamped against the factory 8 fps.
    CHECK(rate < exposure);

    CHECK(Find(plan, "ExposureTime")->value == "150000"); // ms -> us
    CHECK(Find(plan, "AcquisitionFrameRate")->value == "3");
    CHECK_FALSE(Find(plan, "ExposureTime")->value_is_string);
    CHECK(Find(plan, "ExposureTime")->strict);
}

TEST_CASE("apply_plan: the throughput margin precedes the frame rate it caps") {
    gox::CameraConfig c = BaseCamera();
    c.network.throughput_safety_margin_pct = 46;
    c.acquisition.frame_rate_hz = 3.0;
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
    const int margin = IndexOf(plan, "NetworkThroughputSafetyMargin");
    REQUIRE(margin >= 0);
    CHECK(Find(plan, "NetworkThroughputSafetyMargin")->value == "46");
    CHECK(margin < IndexOf(plan, "AcquisitionFrameRate"));

    // 0 means "keep the factory 92": no write at all.
    gox::CameraConfig factory = BaseCamera();
    factory.network.throughput_safety_margin_pct = 0;
    CHECK(Find(gox::BuildApplyPlan(factory), "NetworkThroughputSafetyMargin") == nullptr);
}

TEST_CASE("apply_plan: an external trigger drives the timing, so no frame rate is written") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.trigger.mode = gox::TriggerMode::kExternal;
    c.acquisition.frame_rate_hz = 3.0; // expected PPS, not a camera setting
    c.acquisition.exposure_ms = 150.0;
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);
    CHECK(IndexOf(plan, "AcquisitionFrameRate") == -1);
    CHECK(IndexOf(plan, "ExposureTime") >= 0);
}

TEST_CASE("apply_plan: the trigger selector is armed before the trigger mode") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.trigger.mode = gox::TriggerMode::kExternal;
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);

    const int selector = IndexOf(plan, "TriggerSelector");
    const int source = IndexOf(plan, "TriggerSource");
    const int activation = IndexOf(plan, "TriggerActivation");
    const int mode = IndexOf(plan, "TriggerMode");
    REQUIRE(selector >= 0);
    REQUIRE(mode >= 0);
    // Otherwise TriggerMode=On arms the factory selector (AcquisitionStart).
    CHECK(selector < source);
    CHECK(source < activation);
    CHECK(activation < mode);

    CHECK(Find(plan, "TriggerSelector")->value == "FrameStart");
    CHECK(Find(plan, "TriggerSelector")->value_is_string);
    CHECK(Find(plan, "TriggerMode")->value == "On");
    // The manual prints only integer values for TriggerActivation (1 = Rising
    // Edge, 2 = Falling Edge, p.142); the entry names are not in it.
    CHECK(Find(plan, "TriggerActivation")->value == "1");
    CHECK_FALSE(Find(plan, "TriggerActivation")->value_is_string);
    // Every trigger write carries operator intent and is verified.
    for (const char *n: {"TriggerSelector", "TriggerSource", "TriggerActivation", "TriggerMode"}) {
        CHECK(Find(plan, n)->strict);
    }

    c.acquisition.trigger.activation = gox::TriggerActivation::kFalling;
    CHECK(Find(gox::BuildApplyPlan(c), "TriggerActivation")->value == "2");
}

TEST_CASE("apply_plan: a numeric TriggerSource is written as an enum value, a name as a name") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.trigger.mode = gox::TriggerMode::kExternal;

    c.acquisition.trigger.source_entry = "24"; // Line5 Opt In, the factory default
    const gox::FeatureWrite *numeric = Find(gox::BuildApplyPlan(c), "TriggerSource");
    REQUIRE(numeric != nullptr);
    CHECK(numeric->value == "24");
    CHECK_FALSE(numeric->value_is_string);

    c.acquisition.trigger.source_entry = "Software";
    const gox::FeatureWrite *symbolic = Find(gox::BuildApplyPlan(c), "TriggerSource");
    REQUIRE(symbolic != nullptr);
    CHECK(symbolic->value == "Software");
    CHECK(symbolic->value_is_string);
}

TEST_CASE("apply_plan: ROI writes the size before the offsets and skips zeros") {
    gox::CameraConfig c = BaseCamera();
    gox::RoiConfig roi;
    roi.width = 2048;
    roi.height = 1504;
    roi.offset_x = 1040;
    roi.offset_y = 752;
    c.acquisition.roi = roi;
    c.acquisition.pixel_format = "BayerRG12Packed";
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);

    // The offsets' maxima depend on the size, so the size goes first.
    CHECK(IndexOf(plan, "Width") < IndexOf(plan, "OffsetX"));
    CHECK(IndexOf(plan, "Height") < IndexOf(plan, "OffsetY"));
    // And the whole window precedes PixelFormat.
    CHECK(IndexOf(plan, "OffsetY") < IndexOf(plan, "PixelFormat"));
    CHECK(Find(plan, "Width")->value == "2048");
    CHECK(Find(plan, "OffsetY")->value == "752");
    CHECK(Find(plan, "Width")->strict); // a clamped ROI is baked into every frame header

    // A zeroed ROI means "keep the camera's value": nothing is written.
    gox::CameraConfig keep = BaseCamera();
    keep.acquisition.roi = gox::RoiConfig{};
    const std::vector<gox::FeatureWrite> none = gox::BuildApplyPlan(keep);
    for (const char *n: {"Width", "Height", "OffsetX", "OffsetY"}) {
        CHECK(IndexOf(none, n) == -1);
    }
}

TEST_CASE("apply_plan: features.raw keeps its order, typing and warn-on-clamp semantics") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.exposure_ms = 10.0;
    c.features.raw.push_back(gox::RawFeature{"BlackLevel", "0", false});
    c.features.raw.push_back(gox::RawFeature{"GevSCPD", "1000", false});
    c.features.raw.push_back(gox::RawFeature{"VideoProcessBypassMode", "On", true});
    const std::vector<gox::FeatureWrite> plan = gox::BuildApplyPlan(c);

    REQUIRE(plan.size() >= 3u);
    const size_t n = plan.size();
    CHECK(plan[n - 3].name == "BlackLevel");
    CHECK(plan[n - 2].name == "GevSCPD");
    CHECK(plan[n - 1].name == "VideoProcessBypassMode");
    CHECK(plan[n - 1].value_is_string);
    // The escape hatch is not operator intent the driver can verify: a camera
    // clamp warns instead of failing the bring-up.
    for (size_t i = n - 3; i < n; ++i) {
        CHECK_FALSE(plan[i].strict);
    }
    // Everything the acquisition block generates is strict.
    CHECK(Find(plan, "ExposureTime")->strict);
}

TEST_CASE("apply_plan: no feature is written twice") {
    gox::CameraConfig c = BaseCamera();
    c.acquisition.pixel_format = "BayerRG12Packed";
    c.acquisition.frame_rate_hz = 3.0;
    c.acquisition.exposure_ms = 150.0;
    c.acquisition.gain = 1.5;
    c.acquisition.trigger.mode = gox::TriggerMode::kExternal;
    gox::RoiConfig roi;
    roi.width = 2048;
    roi.height = 1504;
    c.acquisition.roi = roi;
    c.network.throughput_safety_margin_pct = 50;

    std::vector<std::string> n = Names(gox::BuildApplyPlan(c));
    const size_t before = n.size();
    std::sort(n.begin(), n.end());
    n.erase(std::unique(n.begin(), n.end()), n.end());
    CHECK(n.size() == before);
}
