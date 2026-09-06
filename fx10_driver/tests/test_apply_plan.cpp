// The camera write ORDER is a hardware contract (apply_plan.hpp); these tests pin
// the order, the strict/required flags and the trigger-branch differences.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apply_plan.h"

using namespace fx10;

namespace {
    int IndexOf(const std::vector<FeatureWrite> &plan, const std::string &node) {
        for (std::size_t i = 0; i < plan.size(); ++i) {
            if (plan[i].node == node) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int CountOf(const std::vector<FeatureWrite> &plan, const std::string &node) {
        return static_cast<int>(std::count_if(plan.begin(), plan.end(),
                                              [&](const FeatureWrite &w) { return w.node == node; }));
    }

    const FeatureWrite &WriteFor(const std::vector<FeatureWrite> &plan, const std::string &node) {
        const int i = IndexOf(plan, node);
        REQUIRE_MESSAGE(i >= 0, "node '" << node << "' is not in the plan");
        return plan.at(static_cast<std::size_t>(i));
    }

    void ExpectOrder(const std::vector<FeatureWrite> &plan, const std::string &first, const std::string &second) {
        const int a = IndexOf(plan, first);
        const int b = IndexOf(plan, second);
        REQUIRE_MESSAGE(a >= 0, first << " missing");
        REQUIRE_MESSAGE(b >= 0, second << " missing");
        CHECK_MESSAGE(a < b, first << " must be written before " << second);
    }

    const std::vector<RawFeature> kNoRaw;
} // namespace

TEST_CASE("plan: binning precedes everything it invalidates") {
    const auto plan = BuildApplyPlan(AcquisitionConfig{}, kNoRaw);
    CHECK(plan.at(0).node == node::kSpatialBinning);
    CHECK(plan.at(1).node == node::kSpectralBinning);
    ExpectOrder(plan, node::kSpectralBinning, node::kPixelFormat);
    ExpectOrder(plan, node::kSpectralBinning, node::kExposureTime);
    ExpectOrder(plan, node::kSpectralBinning, node::kFrameRateEnable);
}

TEST_CASE("plan: binning values come from the config") {
    AcquisitionConfig acq;
    acq.spatial_binning = 2;
    acq.spectral_binning = 4;
    const auto plan = BuildApplyPlan(acq, kNoRaw);
    CHECK(WriteFor(plan, node::kSpatialBinning).kind == WriteKind::kInt);
    CHECK(WriteFor(plan, node::kSpatialBinning).int_value == 2);
    CHECK(WriteFor(plan, node::kSpectralBinning).int_value == 4);
}

TEST_CASE("plan: MROI is programmed as an index register, then enabled") {
    AcquisitionConfig acq;
    acq.spatial_binning = 1;
    acq.spectral_binning = 1;
    acq.mroi.enabled = true;
    acq.mroi.multiband_string = "100 40;200 60";
    const auto plan = BuildApplyPlan(acq, kNoRaw);

    const int i0 = IndexOf(plan, node::kMroiIndex);
    REQUIRE(i0 >= 0);
    CHECK(plan.at(i0 + 0).int_value == 0);
    CHECK(plan.at(i0 + 1).node == node::kMroiY);
    CHECK(plan.at(i0 + 1).int_value == 100);
    CHECK(plan.at(i0 + 2).node == node::kMroiH);
    CHECK(plan.at(i0 + 2).int_value == 40);
    CHECK(plan.at(i0 + 3).node == node::kMroiIndex);
    CHECK(plan.at(i0 + 3).int_value == 1);
    CHECK(plan.at(i0 + 4).int_value == 200);
    CHECK(plan.at(i0 + 5).int_value == 60);
    CHECK(plan.at(i0 + 6).node == node::kMroiEnable);
    CHECK(plan.at(i0 + 6).bool_value);
    ExpectOrder(plan, node::kSpatialBinning, node::kMroiIndex);
}

TEST_CASE("plan: MROI off is written explicitly (it survives a power cycle)") {
    const auto plan = BuildApplyPlan(AcquisitionConfig{}, kNoRaw);
    REQUIRE(CountOf(plan, node::kMroiEnable) == 1);
    CHECK_FALSE(WriteFor(plan, node::kMroiEnable).bool_value);
    CHECK(IndexOf(plan, node::kMroiIndex) == -1);
}

TEST_CASE("plan: status line is always written (it replaces the last image row)") {
    AcquisitionConfig acq;
    acq.status_line = false;
    CHECK_FALSE(WriteFor(BuildApplyPlan(acq, kNoRaw), node::kStatusLine).bool_value);
    acq.status_line = true;
    CHECK(WriteFor(BuildApplyPlan(acq, kNoRaw), node::kStatusLine).bool_value);
}

TEST_CASE("plan: ExposureMode precedes ExposureTime and the unit is microseconds") {
    AcquisitionConfig acq;
    acq.exposure_ms = 2.5;
    const auto plan = BuildApplyPlan(acq, kNoRaw);
    ExpectOrder(plan, node::kExposureMode, node::kExposureTime);
    CHECK(WriteFor(plan, node::kExposureMode).text == "Timed");
    const FeatureWrite &t = WriteFor(plan, node::kExposureTime);
    CHECK(t.kind == WriteKind::kFloat);
    CHECK(t.float_value == doctest::Approx(2500.0));
    CHECK(t.strict);
    CHECK_FALSE(t.clamp_to_node_range);
}

TEST_CASE("plan: pulse-width exposure writes no ExposureTime") {
    AcquisitionConfig acq;
    acq.trigger.mode = TriggerMode::kExternal;
    acq.trigger.exposure_control = ExposureControl::kPulseWidth;
    const auto plan = BuildApplyPlan(acq, kNoRaw);
    CHECK(WriteFor(plan, node::kExposureMode).text == "TriggerControlled");
    CHECK(IndexOf(plan, node::kExposureTime) == -1);
}

TEST_CASE("plan: pulse width only applies to an external trigger") {
    AcquisitionConfig acq;
    acq.trigger.mode = TriggerMode::kFreerun;
    acq.trigger.exposure_control = ExposureControl::kPulseWidth;
    const auto plan = BuildApplyPlan(acq, kNoRaw);
    CHECK(WriteFor(plan, node::kExposureMode).text == "Timed");
    CHECK(IndexOf(plan, node::kExposureTime) >= 0);
}

TEST_CASE("plan: freerun enables the constant frame rate and clamps it") {
    AcquisitionConfig acq;
    acq.trigger.mode = TriggerMode::kFreerun;
    acq.frame_rate_hz = 120.0;
    const auto plan = BuildApplyPlan(acq, kNoRaw);

    ExpectOrder(plan, node::kTriggerSelector, node::kTriggerMode);
    CHECK(WriteFor(plan, node::kTriggerMode).text == "Off");
    CHECK(WriteFor(plan, node::kFrameRateEnable).bool_value);
    const FeatureWrite &rate = WriteFor(plan, node::kFrameRate);
    CHECK(rate.float_value == doctest::Approx(120.0));
    CHECK(rate.clamp_to_node_range);
    ExpectOrder(plan, node::kFrameRateEnable, node::kFrameRate);
    CHECK(IndexOf(plan, node::kTriggerSource) == -1);
    CHECK(IndexOf(plan, node::kTriggerDelay) == -1);
    CHECK(IndexOf(plan, node::kMissedTriggerSource) == -1);
}

TEST_CASE("plan: external trigger disables the constant frame rate") {
    AcquisitionConfig acq;
    acq.trigger.mode = TriggerMode::kExternal;
    const auto plan = BuildApplyPlan(acq, kNoRaw);
    const FeatureWrite &enable = WriteFor(plan, node::kFrameRateEnable);
    CHECK(enable.kind == WriteKind::kBool);
    CHECK_FALSE(enable.bool_value);
    CHECK(enable.required);
    CHECK(IndexOf(plan, node::kFrameRate) == -1);
}

TEST_CASE("plan: external trigger writes selector, source, activation, then mode") {
    AcquisitionConfig acq;
    acq.trigger.mode = TriggerMode::kExternal;
    acq.trigger.activation = TriggerActivation::kFalling;
    acq.trigger.delay_ms = 1.5;
    acq.trigger.selector_entry = "FrameStart";
    acq.trigger.source_entry = "Line0";
    const auto plan = BuildApplyPlan(acq, kNoRaw);

    ExpectOrder(plan, node::kTriggerSelector, node::kTriggerSource);
    ExpectOrder(plan, node::kTriggerSource, node::kTriggerMode);
    ExpectOrder(plan, node::kTriggerActivation, node::kTriggerMode);
    CHECK(WriteFor(plan, node::kTriggerSelector).text == "FrameStart");
    CHECK(WriteFor(plan, node::kTriggerSource).text == "Line0");
    CHECK(WriteFor(plan, node::kTriggerActivation).text == "FallingEdge");
    CHECK(WriteFor(plan, node::kTriggerDelay).float_value == doctest::Approx(1500.0));
    CHECK(WriteFor(plan, node::kTriggerMode).text == "On");

    acq.trigger.activation = TriggerActivation::kRising;
    CHECK(WriteFor(BuildApplyPlan(acq, kNoRaw), node::kTriggerActivation).text == "RisingEdge");
}

TEST_CASE("plan: the missed-trigger counter is bound after the trigger and is optional") {
    AcquisitionConfig acq;
    acq.trigger.mode = TriggerMode::kExternal;
    const auto plan = BuildApplyPlan(acq, kNoRaw);
    ExpectOrder(plan, node::kTriggerMode, node::kMissedTriggerSource);
    ExpectOrder(plan, node::kMissedTriggerSource, node::kMissedTriggerReset);
    const FeatureWrite &src = WriteFor(plan, node::kMissedTriggerSource);
    CHECK(src.kind == WriteKind::kEnum);
    CHECK(src.text == "MissedTrigger");
    CHECK_FALSE(src.required);
    const FeatureWrite &reset = WriteFor(plan, node::kMissedTriggerReset);
    CHECK(reset.kind == WriteKind::kCommand);
    CHECK_FALSE(reset.required);
}

TEST_CASE("plan: acquisition mode is always Continuous, before the trigger group") {
    for (const TriggerMode mode: {TriggerMode::kFreerun, TriggerMode::kExternal}) {
        AcquisitionConfig acq;
        acq.trigger.mode = mode;
        const auto plan = BuildApplyPlan(acq, kNoRaw);
        CHECK(WriteFor(plan, node::kAcquisitionMode).text == "Continuous");
        ExpectOrder(plan, node::kAcquisitionMode, node::kTriggerSelector);
    }
}

TEST_CASE("plan: raw features come last, in order, with their own type") {
    std::vector<RawFeature> raw;
    raw.push_back({"LineSelector", "enum", "Line1", 0, 0.0, false});
    raw.push_back({"LineSource", "enum", "ExposureActive", 0, 0.0, false});
    raw.push_back({"SomeInt", "int", "7", 7, 0.0, false});
    raw.push_back({"SomeFloat", "float", "1.5", 0, 1.5, false});
    raw.push_back({"SomeBool", "bool", "true", 0, 0.0, true});
    raw.push_back({"SomeString", "string", "hello", 0, 0.0, false});
    raw.push_back({"DeviceReset", "command", "", 0, 0.0, false});
    const auto plan = BuildApplyPlan(AcquisitionConfig{}, raw);

    REQUIRE(plan.size() >= 7u);
    const std::size_t first_raw = plan.size() - 7;
    CHECK(plan[first_raw + 0].node == "LineSelector"); // selector before source, as configured
    CHECK(plan[first_raw + 1].node == "LineSource");
    CHECK(plan[first_raw + 0].kind == WriteKind::kEnum);
    CHECK(plan[first_raw + 0].text == "Line1");
    CHECK(plan[first_raw + 2].kind == WriteKind::kInt);
    CHECK(plan[first_raw + 2].int_value == 7);
    CHECK(plan[first_raw + 3].kind == WriteKind::kFloat);
    CHECK(plan[first_raw + 3].float_value == doctest::Approx(1.5));
    CHECK(plan[first_raw + 4].kind == WriteKind::kBool);
    CHECK(plan[first_raw + 4].bool_value);
    CHECK(plan[first_raw + 5].kind == WriteKind::kString);
    CHECK(plan[first_raw + 5].text == "hello");
    CHECK(plan[first_raw + 6].kind == WriteKind::kCommand);
    for (std::size_t i = first_raw; i < plan.size(); ++i) {
        CHECK_FALSE(plan[i].strict); // a camera-side clamp only warns
        CHECK(plan[i].required); // a node that does not exist is the operator's typo
    }
}

TEST_CASE("plan: every structured write is strict and required except the counter") {
    AcquisitionConfig acq;
    acq.trigger.mode = TriggerMode::kExternal;
    for (const FeatureWrite &w: BuildApplyPlan(acq, kNoRaw)) {
        CAPTURE(w.node);
        CHECK(w.strict);
        const bool optional = w.node == node::kMissedTriggerSource || w.node == node::kMissedTriggerReset;
        CHECK(w.required == !optional);
    }
}

TEST_CASE("plan: only the frame rate is clamped against the node range") {
    AcquisitionConfig acq;
    acq.trigger.mode = TriggerMode::kFreerun;
    for (const FeatureWrite &w: BuildApplyPlan(acq, kNoRaw)) {
        CAPTURE(w.node);
        CHECK(w.clamp_to_node_range == (w.node == node::kFrameRate));
    }
}
