// Tests for the strict config parser (core/config.cpp): defaults, unknown-key
// detection with JSON path + typo suggestion, type/enum/range validation and
// the global -> per-camera deep-merge of the ptp/recording sections.

#include "core/config.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

using ordered_json = nlohmann::ordered_json;

namespace {

jai::AppConfig parse_ok(const std::string& text) {
    return jai::load_config_json(ordered_json::parse(text));
}

// Returns the ConfigError message, or "" if the config unexpectedly parsed.
std::string error_of(const std::string& text) {
    try {
        jai::load_config_json(ordered_json::parse(text));
    } catch (const jai::ConfigError& e) {
        return e.what();
    }
    return {};
}

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// Minimal valid document with an extra top-level member spliced in.
std::string doc_with_top(const std::string& extra) {
    return std::string(R"({"version": 1, )") + extra +
           R"(, "cameras": [{"id": "cam0", "selector": {"by": "ip", "value": "10.0.0.2"}}]})";
}

// Minimal valid document with an extra member spliced into cameras[0].
std::string doc_with_camera(const std::string& extra) {
    return std::string(R"({"version": 1, "cameras": [{"id": "cam0", )") +
           R"("selector": {"by": "ip", "value": "10.0.0.2"}, )" + extra + "}]}";
}

} // namespace

TEST_CASE("config: minimal valid config fills every default") {
    const jai::AppConfig cfg = parse_ok(
        R"({"version": 1, "cameras": [{"id": "cam0", "selector": {"by": "ip", "value": "192.168.10.5"}}]})");
    CHECK(cfg.version == 1);
    CHECK(cfg.logging.level == jai::LogLevel::Info);
    CHECK(cfg.stats.enabled);
    CHECK(cfg.stats.interval_s == doctest::Approx(5.0));
    CHECK(cfg.acquisition.max_frames == 0u);
    CHECK(cfg.acquisition.max_duration_s == doctest::Approx(0.0));
    CHECK(cfg.preflight.fail_on_error);
    CHECK(cfg.ptp.enabled);
    CHECK(cfg.ptp.feature_set == "auto");
    CHECK(cfg.ptp.required_status == "Slave");
    CHECK(cfg.ptp.sync_timeout_s == doctest::Approx(60.0));
    CHECK(cfg.ptp.on_timeout == "abort");
    CHECK(cfg.recording.output_dir == "/workspace/dataset/captures");
    CHECK(cfg.recording.session_name == "auto");
    CHECK(cfg.recording.segment_size_gib == doctest::Approx(2.0));
    CHECK(cfg.recording.record_align == 4096u);
    CHECK(cfg.recording.payload_crc == "none");
    CHECK(cfg.recording.queue_max_frames == 32u);
    CHECK(cfg.recording.queue_on_full == "drop_newest");
    CHECK(cfg.recording.on_buffer_error == "record_flagged");
    CHECK(cfg.recording.flush_interval_mb == 64u);
    CHECK(cfg.recording.min_free_gib == doctest::Approx(10.0));

    REQUIRE(cfg.cameras.size() == 1u);
    const jai::CameraConfig& cam = cfg.cameras[0];
    CHECK(cam.id == "cam0");
    CHECK(cam.enabled);
    CHECK(cam.selector.by == "ip");
    CHECK(cam.selector.value == "192.168.10.5");
    CHECK(cam.discovery.timeout_ms == 4000u);
    CHECK(cam.discovery.retries == 3u);
    CHECK_FALSE(cam.discovery.force_ip.enabled);
    CHECK_FALSE(cam.convenience.exposure_us.has_value());
    CHECK_FALSE(cam.convenience.roi.has_value());
    CHECK_FALSE(cam.convenience.trigger.has_value());
    CHECK(cam.genicam_features.empty());
    CHECK(cam.apply.verify_readback);
    CHECK(cam.stream.channel == 0u);
    CHECK(cam.stream.buffer_count == 0u);
    CHECK(cam.stream.socket_rx_buffer_mib == 16u);
    // With no overrides the per-camera ptp/recording equal the global defaults.
    CHECK(cam.ptp.sync_timeout_s == doctest::Approx(60.0));
    CHECK(cam.recording.record_align == 4096u);
    // Raw config is preserved verbatim for session.json embedding.
    CHECK(cfg.raw.at("version") == 1);
    CHECK(cfg.raw.contains("cameras"));
}

TEST_CASE("config: unknown keys are fatal, with JSON path and a typo suggestion") {
    struct Case {
        std::string json;
        const char* path;    // where the parser must point
        const char* bad_key; // the offending key
        const char* hint;    // the "did you mean" suggestion
    };
    const Case cases[] = {
        {R"({"version": 1, "camras": [],)"
         R"( "cameras": [{"id": "cam0", "selector": {"by": "ip", "value": "10.0.0.2"}}]})",
         "<root>", "camras", "cameras"},
        {doc_with_top(R"("logging": {"levle": "info"})"), "logging", "levle", "level"},
        {doc_with_top(R"("stats": {"intervall_s": 5})"), "stats", "intervall_s", "interval_s"},
        {doc_with_top(R"("acquisition": {"max_frame": 10})"), "acquisition", "max_frame", "max_frames"},
        {doc_with_top(R"("preflight": {"fail_on_err": true})"), "preflight", "fail_on_err",
         "fail_on_error"},
        {doc_with_top(R"("ptp": {"sync_timeout": 30})"), "ptp", "sync_timeout", "sync_timeout_s"},
        {doc_with_top(R"("recording": {"segment_size_gb": 2})"), "recording", "segment_size_gb",
         "segment_size_gib"},
        {doc_with_camera(R"("streem": {})"), "cameras[0]", "streem", "stream"},
        {doc_with_camera(R"("stream": {"buffr_count": 16})"), "cameras[0].stream", "buffr_count",
         "buffer_count"},
        {R"({"version": 1, "cameras": [)"
         R"({"id": "cam0", "selector": {"by": "ip", "value": "10.0.0.2", "byy": "x"}}]})",
         "cameras[0].selector", "byy", "by"},
        {doc_with_camera(R"("discovery": {"force_ip": {"enabled": false, "gatway": "1.1.1.1"}})"),
         "cameras[0].discovery.force_ip", "gatway", "gateway"},
        {doc_with_camera(R"("convenience": {"roi": {"offsetx": 8}})"), "cameras[0].convenience.roi",
         "offsetx", "offset_x"},
        {doc_with_camera(R"("convenience": {"trigger": {"enabld": true}})"),
         "cameras[0].convenience.trigger", "enabld", "enabled"},
        {doc_with_camera(R"("apply": {"verify_readbck": true})"), "cameras[0].apply", "verify_readbck",
         "verify_readback"},
        {doc_with_camera(R"("genicam_features": [{"feature": "X", "value": 1, "onerror": "warn"}])"),
         "cameras[0].genicam_features[0]", "onerror", "on_error"},
        // Unknown keys inside a per-camera override are reported against the
        // camera-scoped path even though the object was deep-merged first.
        {doc_with_camera(R"("ptp": {"on_timeot": "abort"})"), "cameras[0].ptp", "on_timeot",
         "on_timeout"},
        {doc_with_camera(R"("recording": {"payload_crc32": "crc32c"})"), "cameras[0].recording",
         "payload_crc32", "payload_crc"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.path);
        CAPTURE(c.bad_key);
        const std::string err = error_of(c.json);
        CHECK(contains(err, std::string("config error at ") + c.path + ":"));
        CHECK(contains(err, std::string("unknown key \"") + c.bad_key + "\""));
        CHECK(contains(err, std::string("did you mean \"") + c.hint + "\"?"));
    }
}

TEST_CASE("config: wrong value types and out-of-range values are rejected") {
    struct Case {
        std::string json;
        const char* expect; // substring that must appear in the error
    };
    const Case cases[] = {
        {doc_with_top(R"("logging": {"level": 5})"), "logging.level"},
        {doc_with_top(R"("logging": {"level": "loud"})"), "expected trace|debug|info|warn|error"},
        {doc_with_top(R"("stats": {"enabled": "yes"})"), "expected true/false"},
        {doc_with_top(R"("stats": {"interval_s": "5"})"), "expected a number"},
        {doc_with_top(R"("stats": {"interval_s": 0})"), "must be > 0"},
        {doc_with_top(R"("acquisition": {"max_frames": -1})"), "expected a non-negative integer"},
        {doc_with_top(R"("acquisition": {"max_frames": 1.5})"), "expected a non-negative integer"},
        {doc_with_top(R"("recording": {"segment_size_gib": "2"})"), "expected a number"},
        {doc_with_top(R"("recording": {"segment_size_gib": 0})"), "must be > 0"},
        {doc_with_top(R"("recording": {"queue_max_frames": 1})"), "must be >= 2"},
        {doc_with_top(R"("ptp": [])"), "expected an object"},
        {R"({"version": 1, "cameras": [{"id": 42, "selector": {"by": "ip", "value": "x"}}]})",
         "cameras[0].id"},
        {R"({"version": 1, "cameras":)"
         R"( [{"id": "cam0", "enabled": "true", "selector": {"by": "ip", "value": "x"}}]})",
         "expected true/false"},
        {R"({"version": 1, "cameras": {}})", "must be a non-empty array"},
        {R"({"version": 1, "cameras": [{"selector": {"by": "ip", "value": "x"}}]})",
         "camera requires \"id\""},
        {R"({"version": 1, "cameras": [{"id": "cam0"}]})", "camera requires \"selector\""},
        {R"({"version": 1, "cameras": [{"id": "bad/name", "selector": {"by": "ip", "value": "x"}}]})",
         "only [A-Za-z0-9_-]"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.json);
        const std::string err = error_of(c.json);
        CHECK(contains(err, "config error at"));
        CHECK(contains(err, c.expect));
    }
}

TEST_CASE("config: only version 1 is accepted") {
    CHECK(parse_ok(R"({"version": 1,)"
                   R"( "cameras": [{"id": "c", "selector": {"by": "ip", "value": "x"}}]})")
              .version == 1);
    CHECK(contains(error_of(R"({"cameras": []})"), "missing required key \"version\""));
    CHECK(contains(error_of(R"({"version": 2,)"
                            R"( "cameras": [{"id": "c", "selector": {"by": "ip", "value": "x"}}]})"),
                   "config version 1 only"));
    // A string "1" is not an integer 1.
    CHECK(contains(error_of(R"({"version": "1",)"
                            R"( "cameras": [{"id": "c", "selector": {"by": "ip", "value": "x"}}]})"),
                   "config version 1 only"));
}

TEST_CASE("config: camera list validation") {
    const std::string dup = R"({"version": 1, "cameras": [
        {"id": "cam0", "selector": {"by": "ip", "value": "10.0.0.2"}},
        {"id": "cam0", "selector": {"by": "ip", "value": "10.0.0.3"}}]})";
    CHECK(contains(error_of(dup), "duplicate camera id \"cam0\""));

    CHECK(contains(error_of(R"({"version": 1, "cameras": []})"), "must be a non-empty array"));

    const std::string all_disabled = R"({"version": 1, "cameras": [
        {"id": "cam0", "enabled": false, "selector": {"by": "ip", "value": "10.0.0.2"}},
        {"id": "cam1", "enabled": false, "selector": {"by": "ip", "value": "10.0.0.3"}}]})";
    CHECK(contains(error_of(all_disabled), "at least one camera must be enabled"));

    // One disabled + one enabled camera is a valid multi-camera config.
    const std::string mixed = R"({"version": 1, "cameras": [
        {"id": "cam0", "enabled": false, "selector": {"by": "ip", "value": "10.0.0.2"}},
        {"id": "cam1", "selector": {"by": "ip", "value": "10.0.0.3"}}]})";
    const jai::AppConfig cfg = parse_ok(mixed);
    REQUIRE(cfg.cameras.size() == 2u);
    CHECK_FALSE(cfg.cameras[0].enabled);
    CHECK(cfg.cameras[1].enabled);
}

TEST_CASE("config: per-camera ptp/recording deep-merge on top of the globals") {
    const std::string text = R"({
        "version": 1,
        "ptp": {"sync_timeout_s": 25, "poll_interval_ms": 200},
        "recording": {"record_align": 512, "queue_max_frames": 8},
        "cameras": [
            {"id": "cam0", "selector": {"by": "ip", "value": "10.0.0.2"},
             "ptp": {"on_timeout": "warn_continue"},
             "recording": {"payload_crc": "crc32c"}},
            {"id": "cam1", "selector": {"by": "serial", "value": "S123"}}
        ]
    })";
    const jai::AppConfig cfg = parse_ok(text);
    REQUIRE(cfg.cameras.size() == 2u);

    // The global view keeps its own values plus defaults.
    CHECK(cfg.ptp.sync_timeout_s == doctest::Approx(25.0));
    CHECK(cfg.ptp.poll_interval_ms == 200u);
    CHECK(cfg.ptp.on_timeout == "abort");
    CHECK(cfg.recording.record_align == 512u);
    CHECK(cfg.recording.payload_crc == "none");

    // cam0 overrides only on_timeout / payload_crc; everything else must come
    // through from the globals (deep-merge, not wholesale replacement).
    const jai::CameraConfig& cam0 = cfg.cameras[0];
    CHECK(cam0.ptp.sync_timeout_s == doctest::Approx(25.0)); // global still effective
    CHECK(cam0.ptp.poll_interval_ms == 200u);
    CHECK(cam0.ptp.on_timeout == "warn_continue"); // override effective
    CHECK(cam0.recording.record_align == 512u);
    CHECK(cam0.recording.queue_max_frames == 8u);
    CHECK(cam0.recording.payload_crc == "crc32c");
    CHECK(cam0.recording.output_dir == "/workspace/dataset/captures"); // untouched default

    // cam1 has no overrides: it inherits the globals verbatim.
    const jai::CameraConfig& cam1 = cfg.cameras[1];
    CHECK(cam1.ptp.sync_timeout_s == doctest::Approx(25.0));
    CHECK(cam1.ptp.on_timeout == "abort");
    CHECK(cam1.recording.record_align == 512u);
    CHECK(cam1.recording.payload_crc == "none");
}

TEST_CASE("config: genicam_features scalar coercion") {
    const std::string text = doc_with_camera(R"("genicam_features": [
        {"feature": "Width", "value": 1936},
        {"feature": "AcquisitionFrameRate", "value": 20.5},
        {"feature": "ReverseX", "value": true},
        {"feature": "UserSetLoad", "value": null},
        {"feature": "PixelFormat", "value": "BayerRG10p", "on_error": "warn"}
    ])");
    const jai::AppConfig cfg = parse_ok(text);
    const std::vector<jai::GenicamFeature>& f = cfg.cameras[0].genicam_features;
    REQUIRE(f.size() == 5u);
    CHECK(f[0].feature == "Width");
    CHECK(f[0].value == "1936"); // integer -> decimal text
    CHECK_FALSE(f[0].value_is_string);
    CHECK(f[1].value == "20.5"); // float -> round-trip text
    CHECK_FALSE(f[1].value_is_string);
    CHECK(f[2].value == "true"); // bool -> "true"/"false"
    CHECK_FALSE(f[2].value_is_string);
    CHECK(f[3].value.empty()); // null -> "" (command features take no value)
    CHECK(f[4].value == "BayerRG10p");
    CHECK(f[4].value_is_string);
    CHECK(f[4].on_error == "warn");
    CHECK(f[0].on_error.empty()); // falls back to apply.on_error_default

    // Non-scalar values and missing members are rejected.
    CHECK(contains(error_of(doc_with_camera(R"("genicam_features": [{"feature": "X", "value": [1]}])")),
                   "expected a scalar"));
    CHECK(contains(error_of(doc_with_camera(R"("genicam_features": [{"feature": "X"}])")),
                   "requires \"feature\" and \"value\""));
    CHECK(contains(error_of(doc_with_camera(R"("genicam_features": [{"feature": "", "value": 1}])")),
                   "must not be empty"));
    CHECK(contains(error_of(doc_with_camera(R"("genicam_features": {})")), "expected an array"));
}

TEST_CASE("config: enum values are validated and the error lists what is allowed") {
    const std::string err = error_of(doc_with_top(R"("recording": {"queue_on_full": "drop_newst"})"));
    CHECK(contains(err, "config error at recording.queue_on_full:"));
    CHECK(contains(err, "invalid value \"drop_newst\""));
    CHECK(contains(err, "\"drop_newest\""));
    CHECK(contains(err, "\"block\""));

    CHECK(contains(error_of(doc_with_top(R"("recording": {"payload_crc": "crc32"})")),
                   "recording.payload_crc"));
    CHECK(contains(error_of(doc_with_top(R"("recording": {"on_buffer_error": "ignore"})")),
                   "recording.on_buffer_error"));
    CHECK(contains(error_of(doc_with_top(R"("ptp": {"feature_set": "bogus"})")), "ptp.feature_set"));
    CHECK(contains(error_of(doc_with_top(R"("ptp": {"on_timeout": "retry"})")), "ptp.on_timeout"));

    // An enum typo inside a per-camera override keeps the camera-scoped path.
    CHECK(contains(error_of(doc_with_camera(R"("recording": {"queue_on_full": "blockk"})")),
                   "cameras[0].recording.queue_on_full"));
}

TEST_CASE("config: mac selector value must be a parseable MAC address") {
    auto cam_with_selector = [](const std::string& by, const std::string& value) {
        return std::string(R"({"version": 1, "cameras": [{"id": "cam0", "selector": {"by": ")") + by +
               R"(", "value": ")" + value + R"("}}]})";
    };
    // All common MAC spellings are accepted; the config keeps the raw text
    // (normalization happens again at discovery time).
    CHECK(parse_ok(cam_with_selector("mac", "00:0C:DF:12:34:56")).cameras[0].selector.value ==
          "00:0C:DF:12:34:56");
    CHECK(parse_ok(cam_with_selector("mac", "00-0c-df-12-34-56")).cameras[0].selector.by == "mac");
    CHECK(parse_ok(cam_with_selector("mac", "000cdf123456")).cameras[0].selector.by == "mac");

    CHECK(contains(error_of(cam_with_selector("mac", "00:0c:df:12:34")), "not a valid MAC address"));
    CHECK(contains(error_of(cam_with_selector("mac", "zz:zz:zz:zz:zz:zz")),
                   "cameras[0].selector.value"));
    CHECK(contains(error_of(cam_with_selector("ip", "")), "must not be empty"));
    CHECK(contains(error_of(cam_with_selector("hostname", "cam.local")), "cameras[0].selector.by"));
    CHECK(contains(error_of(R"({"version": 1, "cameras": [{"id": "cam0", "selector": {}}]})"),
                   "selector requires \"by\" and \"value\""));
}

TEST_CASE("config: record_align must be a power of two") {
    CHECK(contains(error_of(doc_with_top(R"("recording": {"record_align": 4095})")), "power of two"));
    CHECK(contains(error_of(doc_with_top(R"("recording": {"record_align": 0})")), "power of two"));
    CHECK(contains(error_of(doc_with_camera(R"("recording": {"record_align": 3})")),
                   "cameras[0].recording.record_align"));
    CHECK(parse_ok(doc_with_top(R"("recording": {"record_align": 1})")).recording.record_align == 1u);
    CHECK(parse_ok(doc_with_top(R"("recording": {"record_align": 512})")).recording.record_align ==
          512u);
}
