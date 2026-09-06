// The session metadata contract (src/device_json.cpp): the shape of
// <cam>/device.json and <cam>/telemetry.jsonl, the factory-value expectation
// tables behind the raw audit, and the manual constants recorded alongside the
// pixels. All SDK-free.

#include "device_json.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>

namespace {
    gox::DeviceReport BaseReport() {
        gox::DeviceReport r;
        r.camera_id = "cam0";
        r.ip = "10.95.1.101";
        r.connection_path = "direct_ip";
        r.created_realtime_ns = 1756900000000000000ull;
        r.factory_load.loaded = true;
        r.factory_load.selector_entry = "Default";
        r.factory_load.selector_readback = "Default";
        r.factory_load.elapsed_ms = 412;
        r.factory_load.is_done_supported = true;
        return r;
    }

    gox::AppliedFeature Applied(std::string name, std::string requested, std::string readback) {
        gox::AppliedFeature a;
        a.name = std::move(name);
        a.requested = std::move(requested);
        a.readback = std::move(readback);
        return a;
    }

    gox::AuditEntry Audit(std::string name, std::string value, std::string expected, bool readable) {
        gox::AuditEntry e;
        e.name = std::move(name);
        e.value = std::move(value);
        e.expected = std::move(expected);
        e.outcome = gox::ClassifyAudit(e.value, e.expected, readable);
        return e;
    }
} // namespace

TEST_CASE("device_json: raw_audit_matches accepts the spellings ToString() can produce") {
    // The same value reaches us as an enum name, a boolean, or a formatted
    // number depending on the node type, and the manual's own casing varies.
    CHECK(gox::RawAuditMatches("Off", "Off"));
    CHECK(gox::RawAuditMatches(" off ", "OFF"));
    CHECK(gox::RawAuditMatches("0", "0.000000"));
    CHECK(gox::RawAuditMatches("1.0", "1"));
    CHECK(gox::RawAuditMatches("False", "Off|false|0"));
    CHECK(gox::RawAuditMatches("0", "Off|false|0"));

    CHECK_FALSE(gox::RawAuditMatches("On", "Off"));
    CHECK_FALSE(gox::RawAuditMatches("1", "0"));
    CHECK_FALSE(gox::RawAuditMatches("Continuous", "Timed"));
    CHECK_FALSE(gox::RawAuditMatches("", "Off"));
}

TEST_CASE("device_json: classify_audit separates a deviation from a missing feature") {
    CHECK(gox::ClassifyAudit("Off", "Off", true) == gox::AuditOutcome::kMatch);
    CHECK(gox::ClassifyAudit("On", "Off", true) == gox::AuditOutcome::kDeviates);
    // Mono-only / RGB-only features: absent is the normal reading, not a fault.
    CHECK(gox::ClassifyAudit("<absent>", "Off", false) == gox::AuditOutcome::kUnreadable);
    // Record-only entries have nothing to judge.
    CHECK(gox::ClassifyAudit("BayerRG12Packed", "", true) == gox::AuditOutcome::kRecorded);

    CHECK(std::string(gox::AuditOutcomeName(gox::AuditOutcome::kMatch)) == "match");
    CHECK(std::string(gox::AuditOutcomeName(gox::AuditOutcome::kDeviates)) == "deviates");
    CHECK(std::string(gox::AuditOutcomeName(gox::AuditOutcome::kRecorded)) == "recorded");
    CHECK(std::string(gox::AuditOutcomeName(gox::AuditOutcome::kUnreadable)) == "unreadable");
}

TEST_CASE("device_json: the expectation tables are complete and free of duplicates") {
    const auto &table = gox::RawAuditExpectations();
    CHECK(table.size() >= 38u); // the pixel path the driver deliberately never writes
    std::set<std::string> seen;
    for (const gox::RawAuditExpectation &x: table) {
        CHECK(std::string(x.name).size() > 0);
        CHECK(seen.insert(x.name).second); // a name audited twice would warn twice
    }
    // The load-bearing entries: bypass must be Off (its whole justification is
    // that the chain is already identity), and the operator's blemish decision
    // is recorded rather than judged.
    const auto find_expected = [&](const std::string &name) -> std::string {
        for (const gox::RawAuditExpectation &x: table) {
            if (name == x.name) {
                return x.expected;
            }
        }
        return "<not in table>";
    };
    CHECK(find_expected("VideoProcessBypassMode") == "Off");
    CHECK(find_expected("BlackLevel") == "0");
    CHECK(find_expected("ExposureMode") == "Timed");
    CHECK(find_expected("AcquisitionMode") == "Continuous");
    CHECK(find_expected("BlemishEnable").empty());
    CHECK(find_expected("PixelFormat").empty());

    // Selector-scoped reads: the two digital gains and the two colour black
    // levels, and both selectors have a home entry to be restored to.
    const auto &selectors = gox::SelectorAuditExpectations();
    CHECK(selectors.size() == 4u);
    std::set<std::string> labels;
    for (const gox::SelectorAuditExpectation &x: selectors) {
        CHECK(labels.insert(x.label).second);
        CHECK(std::string(x.entry).find_first_not_of("0123456789") == std::string::npos);
    }
    CHECK(labels.count("Gain[DigitalRed]") == 1u);
    CHECK(labels.count("BlackLevel[Blue]") == 1u);
    CHECK(gox::SelectorHomeEntries().size() == 2u);

    CHECK(gox::IdentityFeatures().size() >= 17u);
    CHECK(gox::TransportFeatures().size() >= 5u);
}

TEST_CASE("device_json: counter0_bound reads the applied plan, not the config") {
    std::vector<gox::AppliedFeature> plan;
    CHECK_FALSE(gox::Counter0Bound(plan)); // freerun: never armed

    plan.push_back(Applied("TriggerMode", "On", "On"));
    plan.push_back(Applied("CounterEventSource", "1", "1"));
    CHECK(gox::Counter0Bound(plan));

    plan.back().readback = "<absent>"; // a camera without counters
    CHECK_FALSE(gox::Counter0Bound(plan));
    plan.back().readback = "<failed>"; // present, but numbered differently
    CHECK_FALSE(gox::Counter0Bound(plan));
}

TEST_CASE("device_json: build_device_json renders a stable, self-describing document") {
    gox::DeviceReport r = BaseReport();
    r.identity.emplace_back("DeviceSerialNumber", "00123");
    r.identity.emplace_back("DeviceModelName", "GOX-12405MC-PGE");
    r.applied.push_back(Applied("PixelFormat", "BayerRG12Packed", "BayerRG12Packed"));
    r.applied.push_back(Applied("ExposureTime", "150000", "150000"));
    r.applied.push_back(Applied("ExposureTime", "120000", "120000")); // features.raw override
    r.applied.push_back(Applied("CounterReset", "", "<executed>"));
    r.applied.back().tolerance_rel = 0.0;
    r.raw_audit.push_back(Audit("LUTMode", "Off", "Off", true));
    r.raw_audit.push_back(Audit("VideoProcessBypassMode", "On", "Off", true));
    r.raw_audit.push_back(Audit("ROICentered", "<absent>", "Off|false|0", false));
    r.raw_audit.push_back(Audit("PixelFormat", "BayerRG12Packed", "", true));
    r.transport.emplace_back("PayloadSize", "18624000");
    r.transport.emplace_back("AcquisitionFrameRate", "3.0");
    r.transport.emplace_back("GevGVSPExtendedIDMode", "On");
    r.frame_rate_min_hz = 0.125;
    r.frame_rate_max_hz = 5.42;
    r.exposure_time_us = 150000.0;
    r.sensor_digitization_bits = 12;
    r.black_level_register = "0";
    r.runtime.buffer_count = 16;
    r.runtime.counter0_bound = true;
    r.ptp.enabled = true;
    r.ptp.synchronized = true;
    r.ptp.status = "slave";
    r.ptp.accuracy = 6;
    r.ptp.have_offset = true;
    r.ptp.raw_offset_ns = -37000000123ll;
    r.ptp.adjusted_offset_ns = -123;
    r.ptp.tai_detected = true;

    const nlohmann::ordered_json doc = gox::BuildDeviceJson(r);

    // A reader must be able to tell what it is looking at from the first key.
    REQUIRE(doc.begin() != doc.end());
    CHECK(doc.begin().key() == "format");
    CHECK(doc["format"] == "gox-device-json/1");
    CHECK(doc["camera_id"] == "cam0");
    CHECK(doc["factory_load"]["selector_entry"] == "Default");
    CHECK(doc["factory_load"]["is_done_supported"] == true);

    // Identity is never coerced: a serial with leading zeros must survive.
    CHECK(doc["identity"]["DeviceSerialNumber"].is_string());
    CHECK(doc["identity"]["DeviceSerialNumber"] == "00123");

    // applied is an array: order is the ordering contract, and features.raw may
    // legally write the same feature again.
    REQUIRE(doc["applied"].is_array());
    CHECK(doc["applied"].size() == 4u);
    CHECK(doc["applied"][0]["name"] == "PixelFormat");
    CHECK(doc["applied"][1]["name"] == "ExposureTime");
    CHECK(doc["applied"][2]["name"] == "ExposureTime");
    CHECK(doc["applied"][3]["readback"] == "<executed>");
    CHECK(doc["applied"][0].contains("tolerance_rel"));

    // raw_audit is an object: one lookup per feature name.
    CHECK(doc["raw_audit"]["LUTMode"]["outcome"] == "match");
    // The documented factory value is recorded on every judged entry, matched
    // or not: a reader should not have to hold the manual to check the claim.
    CHECK(doc["raw_audit"]["LUTMode"]["expected"] == "Off");
    CHECK(doc["raw_audit"]["VideoProcessBypassMode"]["outcome"] == "deviates");
    CHECK(doc["raw_audit"]["VideoProcessBypassMode"]["expected"] == "Off");
    CHECK(doc["raw_audit"]["ROICentered"]["value"] == "<absent>");
    CHECK(doc["raw_audit"]["ROICentered"]["outcome"] == "unreadable");
    // A record-only entry has no documented factory value, so no `expected` key.
    CHECK(doc["raw_audit"]["PixelFormat"]["outcome"] == "recorded");
    CHECK_FALSE(doc["raw_audit"]["PixelFormat"].contains("expected"));

    // Transport numbers stay numbers; a transport enum stays a string.
    CHECK(doc["transport"]["PayloadSize"] == 18624000);
    CHECK(doc["transport"]["AcquisitionFrameRate"].get<double>() == doctest::Approx(3.0));
    CHECK(doc["transport"]["GevGVSPExtendedIDMode"].is_string());
    CHECK(doc["transport"]["AcquisitionFrameRateMax"].get<double>() == doctest::Approx(5.42));

    // Derived: the sensor's own exposure offset, and a pedestal that is a
    // specification value rather than a register read.
    CHECK(doc["derived"]["exposure_offset_us"].get<double>() == doctest::Approx(gox::kExposureOffsetUs));
    CHECK(doc["derived"]["actual_integration_us"].get<double>() == doctest::Approx(150002.45));
    CHECK(doc["derived"]["exposure_time_max_us"].is_null()); // never read
    CHECK(doc["derived"]["black_level"]["documented_pedestal"] == "8LSB@8bit");
    CHECK(doc["derived"]["black_level"]["adjustment_resolution"] == "1LSB@12bit");
    CHECK(doc["derived"]["black_level"]["pedestal_lsb8"] == 8);
    CHECK(doc["derived"]["black_level"]["pedestal_lsb12_derived"] == 128);
    CHECK(doc["derived"]["black_level"]["derived_by_driver"] == true);
    CHECK(doc["derived"]["sensor_digitization_bits"] == 12);
    CHECK(doc["derived"]["thermal_limit_c"].get<double>() == doctest::Approx(72.0));

    CHECK(doc["runtime"]["buffer_count"] == 16);
    CHECK(doc["runtime"]["counter0_bound"] == true);

    // PTP: the 37 s is the driver's assumption about the grandmaster, not a
    // camera property, so both numbers and the assumption are recorded.
    CHECK(doc["ptp"]["status"] == "slave");
    CHECK(doc["ptp"]["accuracy"] == 6);
    CHECK(doc["ptp"]["raw_offset_ns"] == -37000000123ll);
    CHECK(doc["ptp"]["adjusted_offset_ns"] == -123);
    CHECK(doc["ptp"]["tai_utc_offset_detected"] == true);
    CHECK(doc["ptp"]["assumed_tai_utc_offset_s"] == 37);
    CHECK(doc["ptp"]["driver_assumption"] == true);
}

TEST_CASE("device_json: an empty report still has every key, with nulls") {
    const nlohmann::ordered_json doc = gox::BuildDeviceJson(gox::DeviceReport{});
    CHECK(doc["identity"].is_object());
    CHECK(doc["applied"].is_array());
    CHECK(doc["applied"].empty());
    CHECK(doc["derived"]["exposure_time_us"].is_null());
    CHECK(doc["derived"]["actual_integration_us"].is_null());
    CHECK(doc["derived"]["sensor_digitization_bits"].is_null());
    // The constants are properties of the camera model, not of this session.
    CHECK(doc["derived"]["exposure_offset_us"].get<double>() == doctest::Approx(2.45));
    CHECK(doc["derived"]["black_level"]["black_level_register"].is_null());
    CHECK(doc["transport"]["AcquisitionFrameRateMin"].is_null());
    CHECK(doc["ptp"]["accuracy"].is_null()); // -1 = never read
    CHECK(doc["ptp"]["raw_offset_ns"].is_null());
}

TEST_CASE("device_json: build_telemetry_line is one parseable row per poll") {
    gox::TelemetrySample s;
    s.hrt = 1756900005123456789ull;
    s.temp_sensor = 42.1;
    s.temp_mainboard = 45.0;
    s.temp_fpga = 51.3;
    s.trig = 123;
    s.trig_overflow = false;
    s.pause_rx = 0;
    s.ptp_status = "slave";
    s.ptp_accuracy = 6;
    s.ptp_offset_ns = -123;

    const std::string line = gox::BuildTelemetryLine(s);
    CHECK(line.find('\n') == std::string::npos); // the writer adds the newline
    const nlohmann::json row = nlohmann::json::parse(line);
    CHECK(row["hrt"] == 1756900005123456789ull);
    CHECK(row["temp"]["sensor"].get<double>() == doctest::Approx(42.1));
    CHECK(row["temp"]["fpga"].get<double>() == doctest::Approx(51.3));
    CHECK(row["trig"] == 123);
    CHECK(row["trig_overflow"] == false);
    CHECK(row["pause_rx"] == 0);
    CHECK(row["ptp"]["status"] == "slave");
    CHECK(row["ptp"]["accuracy"] == 6);
    CHECK(row["ptp"]["offset_ns"] == -123);
}

TEST_CASE("device_json: a telemetry row keeps its shape when the camera answers nothing") {
    const std::string line = gox::BuildTelemetryLine(gox::TelemetrySample{});
    const nlohmann::json row = nlohmann::json::parse(line);
    CHECK(row["hrt"] == 0);
    CHECK(row["temp"]["sensor"].is_null());
    CHECK(row["temp"]["mainboard"].is_null());
    CHECK(row["temp"]["fpga"].is_null());
    CHECK(row["trig"].is_null()); // freerun, or the counter never bound
    CHECK(row["pause_rx"].is_null());
    CHECK(row["ptp"]["status"].is_null());
    CHECK(row["ptp"]["accuracy"].is_null());
    CHECK(row["ptp"]["offset_ns"].is_null());
}
