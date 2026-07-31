// Tests for the lenient YAML config parser (core/config.cpp): defaults for
// absent keys, unknown keys ignored, the critical-invariant validations that
// survived from the strict-JSON era, and the global -> per-camera key-by-key
// merge of the ptp/recording sections.

#include "../include/config.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {
    jai::AppConfig parse_ok(const std::string &text) {
        return jai::load_config_text(text);
    }

    // Returns the ConfigError message, or "" if the config unexpectedly parsed.
    std::string error_of(const std::string &text) {
        try {
            jai::load_config_text(text);
        } catch (const jai::ConfigError &e) {
            return e.what();
        }
        return {};
    }

    bool contains(const std::string &s, const std::string &needle) {
        return s.find(needle) != std::string::npos;
    }

    const std::string kMinimalCameras =
            "cameras:\n"
            "  - id: cam0\n"
            "    selector: {by: ip, value: 10.0.0.2}\n";

    // Minimal valid document with extra top-level YAML spliced in.
    std::string doc_with_top(const std::string &extra) {
        return "version: 1\n" + extra + "\n" + kMinimalCameras;
    }

    // Minimal valid document with extra keys spliced into cameras[0] (must be
    // indented 4 spaces to sit inside the camera map).
    std::string doc_with_camera(const std::string &extra) {
        return "version: 1\n" + kMinimalCameras + extra + "\n";
    }
} // namespace

TEST_CASE("config: minimal valid config fills every default") {
    const std::string text =
            "version: 1\n"
            "cameras:\n"
            "  - id: cam0\n"
            "    selector: {by: ip, value: 192.168.10.5}\n";
    const jai::AppConfig cfg = parse_ok(text);
    CHECK(cfg.version == 1);
    CHECK(cfg.stats_interval_s == doctest::Approx(5.0));
    CHECK(cfg.acquisition.max_frames == 0u);
    CHECK(cfg.acquisition.max_duration_s == doctest::Approx(0.0));
    CHECK_FALSE(cfg.ptp.enabled); // PTP is opt-in
    CHECK(cfg.ptp.feature_set == "auto");
    CHECK(cfg.ptp.required_status == "Slave");
    CHECK(cfg.ptp.sync_timeout_s == doctest::Approx(60.0));
    CHECK(cfg.ptp.on_timeout == "abort");
    CHECK(cfg.recording.output_dir == "/workspace/dataset/captures");
    CHECK(cfg.recording.session_name == "auto");
    CHECK(cfg.recording.segment_size_gib == doctest::Approx(2.0));
    CHECK(cfg.recording.record_align == 4096u);
    CHECK(cfg.recording.queue_max_frames == 32u);
    CHECK(cfg.recording.queue_on_full == "drop_newest");
    CHECK(cfg.recording.on_buffer_error == "record_flagged");
    CHECK(cfg.recording.flush_interval_mb == 64u);
    CHECK(cfg.recording.min_free_gib == doctest::Approx(10.0));

    REQUIRE(cfg.cameras.size() == 1u);
    const jai::CameraConfig &cam = cfg.cameras[0];
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
}

TEST_CASE("config: unknown keys are ignored, absent version defaults to 1") {
    const std::string text =
            "bogus_top_level: 42\n"
            "logging: {stats_intervall_s: 99}\n" // typo key: ignored
            "cameras:\n"
            "  - id: cam0\n"
            "    selector: {by: ip, value: 10.0.0.2}\n"
            "    streem: {buffer_count: 2}\n"; // typo key: ignored, no range check
    const jai::AppConfig cfg = parse_ok(text);
    CHECK(cfg.version == 1); // no version key: default accepted
    CHECK(cfg.stats_interval_s == doctest::Approx(5.0)); // typo left the default
    CHECK(cfg.cameras[0].stream.buffer_count == 0u);
}

TEST_CASE("config: critical invariants are still rejected") {
    struct Case {
        std::string yaml;
        const char *expect; // substring that must appear in the error
    };
    const Case cases[] = {
        {doc_with_top("logging: {stats_interval_s: 0}"), "logging.stats_interval_s must be > 0"},
        {doc_with_top("ptp: {poll_interval_ms: 0}"), "ptp.poll_interval_ms must be > 0"},
        {
            doc_with_top("ptp: {feature_set: explicit}"),
            "requires enable_feature and status_feature"
        },
        {doc_with_top("recording: {segment_size_gib: 0}"), "segment_size_gib must be > 0"},
        {doc_with_top("recording: {queue_max_frames: 1}"), "must be >= 2"},
        {doc_with_top("recording: {flush_interval_mb: 0}"), "flush_interval_mb must be > 0"},
        {doc_with_camera("    stream: {buffer_count: 2}"), "must be 0 (auto) or >= 4"},
        {doc_with_camera("    stream: {packet_size: 100}"), "in [576, 16000]"},
        {doc_with_camera("    stream: {socket_rx_buffer_mib: 0}"), "must be > 0"},
        {
            doc_with_camera("    discovery: {force_ip: {enabled: true}}"),
            "requires ip and subnet_mask"
        },
        {
            "version: 1\ncameras:\n  - id: bad/name\n    selector: {by: ip, value: x}\n",
            "only [A-Za-z0-9_-]"
        },
        {"version: 1\ncameras:\n  - selector: {by: ip, value: x}\n", "id is required"},
        {"version: 1\ncameras:\n  - id: cam0\n", "selector requires \"by\" and \"value\""},
    };
    for (const Case &c: cases) {
        CAPTURE(c.yaml);
        CHECK(contains(error_of(c.yaml), c.expect));
    }
}

TEST_CASE("config: only version 1 is accepted when present") {
    CHECK(parse_ok(doc_with_top("")).version == 1);
    CHECK(contains(error_of("version: 2\n" + kMinimalCameras), "config version 1 only"));
}

TEST_CASE("config: camera list validation") {
    const std::string dup =
            "version: 1\n"
            "cameras:\n"
            "  - {id: cam0, selector: {by: ip, value: 10.0.0.2}}\n"
            "  - {id: cam0, selector: {by: ip, value: 10.0.0.3}}\n";
    CHECK(contains(error_of(dup), "duplicate camera id \"cam0\""));

    CHECK(contains(error_of("version: 1\ncameras: []\n"), "non-empty list"));
    CHECK(contains(error_of("version: 1\n"), "non-empty list"));
    CHECK(contains(error_of(""), "empty document"));

    const std::string all_disabled =
            "version: 1\n"
            "cameras:\n"
            "  - {id: cam0, enabled: false, selector: {by: ip, value: 10.0.0.2}}\n"
            "  - {id: cam1, enabled: false, selector: {by: ip, value: 10.0.0.3}}\n";
    CHECK(contains(error_of(all_disabled), "at least one camera must be enabled"));

    // One disabled + one enabled camera is a valid multi-camera config.
    const std::string mixed =
            "version: 1\n"
            "cameras:\n"
            "  - {id: cam0, enabled: false, selector: {by: ip, value: 10.0.0.2}}\n"
            "  - {id: cam1, selector: {by: ip, value: 10.0.0.3}}\n";
    const jai::AppConfig cfg = parse_ok(mixed);
    REQUIRE(cfg.cameras.size() == 2u);
    CHECK_FALSE(cfg.cameras[0].enabled);
    CHECK(cfg.cameras[1].enabled);
}

TEST_CASE("config: per-camera ptp/recording merge on top of the globals") {
    const std::string text =
            "version: 1\n"
            "ptp: {enabled: true, sync_timeout_s: 25, poll_interval_ms: 200}\n"
            "recording: {record_align: 512, queue_max_frames: 8}\n"
            "cameras:\n"
            "  - id: cam0\n"
            "    selector: {by: ip, value: 10.0.0.2}\n"
            "    ptp: {on_timeout: warn_continue}\n"
            "    recording: {flush_interval_mb: 128}\n"
            "  - id: cam1\n"
            "    selector: {by: serial, value: S123}\n";
    const jai::AppConfig cfg = parse_ok(text);
    REQUIRE(cfg.cameras.size() == 2u);

    // The global view keeps its own values plus defaults.
    CHECK(cfg.ptp.sync_timeout_s == doctest::Approx(25.0));
    CHECK(cfg.ptp.poll_interval_ms == 200u);
    CHECK(cfg.ptp.on_timeout == "abort");
    CHECK(cfg.recording.record_align == 512u);
    CHECK(cfg.recording.flush_interval_mb == 64u);

    // cam0 overrides only on_timeout / flush_interval_mb; everything else must
    // come through from the globals (key-by-key merge, not wholesale replacement).
    const jai::CameraConfig &cam0 = cfg.cameras[0];
    CHECK(cam0.ptp.sync_timeout_s == doctest::Approx(25.0)); // global still effective
    CHECK(cam0.ptp.poll_interval_ms == 200u);
    CHECK(cam0.ptp.on_timeout == "warn_continue"); // override effective
    CHECK(cam0.recording.record_align == 512u);
    CHECK(cam0.recording.queue_max_frames == 8u);
    CHECK(cam0.recording.flush_interval_mb == 128u);
    CHECK(cam0.recording.output_dir == "/workspace/dataset/captures"); // untouched default

    // cam1 has no overrides: it inherits the globals verbatim. `enabled` is
    // the regression guard for the parse-order bug where cameras were parsed
    // BEFORE the global ptp block and snapshotted the struct defaults.
    const jai::CameraConfig &cam1 = cfg.cameras[1];
    CHECK(cam1.ptp.enabled);
    CHECK(cam1.ptp.sync_timeout_s == doctest::Approx(25.0));
    CHECK(cam1.ptp.on_timeout == "abort");
    CHECK(cam1.recording.record_align == 512u);
    CHECK(cam1.recording.flush_interval_mb == 64u);

    // An invalid value inside a per-camera override keeps the camera path.
    CHECK(contains(error_of(doc_with_camera("    recording: {record_align: 3}")),
        "cameras[0].recording.record_align"));
}

TEST_CASE("config: genicam_features scalar typing follows YAML quoting") {
    const std::string text = doc_with_camera(
        "    genicam_features:\n"
        "      - {feature: Width, value: 1936}\n"
        "      - {feature: AcquisitionFrameRate, value: 20.5}\n"
        "      - {feature: ReverseX, value: true}\n"
        "      - {feature: UserSetLoad, value: ~}\n"
        "      - {feature: PixelFormat, value: \"BayerRG10p\", on_error: warn}\n"
        "      - {feature: UserSetSave}\n");
    const jai::AppConfig cfg = parse_ok(text);
    const std::vector<jai::GenicamFeature> &f = cfg.cameras[0].genicam_features;
    REQUIRE(f.size() == 6u);
    CHECK(f[0].feature == "Width");
    CHECK(f[0].value == "1936"); // plain scalar -> typed by GenICam node type
    CHECK_FALSE(f[0].value_is_string);
    CHECK(f[1].value == "20.5");
    CHECK_FALSE(f[1].value_is_string);
    CHECK(f[2].value == "true");
    CHECK_FALSE(f[2].value_is_string);
    CHECK(f[3].value.empty()); // null -> "" (command features take no value)
    CHECK(f[4].value == "BayerRG10p"); // quoted -> apply as string/enum entry
    CHECK(f[4].value_is_string);
    CHECK(f[4].on_error == "warn");
    CHECK(f[5].value.empty()); // absent value: also a command feature
    CHECK(f[0].on_error.empty()); // falls back to apply.on_error_default

    // A missing/empty feature name is rejected.
    CHECK(contains(error_of(doc_with_camera("    genicam_features: [{value: 1}]")),
        "non-empty \"feature\""));
    CHECK(contains(error_of(doc_with_camera("    genicam_features: [{feature: \"\", value: 1}]")),
        "non-empty \"feature\""));
}

TEST_CASE("config: enum values are validated and the error lists what is allowed") {
    const std::string err = error_of(doc_with_top("recording: {queue_on_full: drop_newst}"));
    CHECK(contains(err, "recording.queue_on_full"));
    CHECK(contains(err, "invalid value \"drop_newst\""));
    CHECK(contains(err, "drop_newest"));
    CHECK(contains(err, "block"));

    CHECK(contains(error_of(doc_with_top("recording: {on_buffer_error: ignore}")),
        "recording.on_buffer_error"));
    CHECK(contains(error_of(doc_with_top("ptp: {feature_set: bogus}")), "ptp.feature_set"));
    CHECK(contains(error_of(doc_with_top("ptp: {on_timeout: retry}")), "ptp.on_timeout"));

    // An enum typo inside a per-camera override keeps the camera-scoped path.
    CHECK(contains(error_of(doc_with_camera("    recording: {queue_on_full: blockk}")),
        "cameras[0].recording.queue_on_full"));
}

TEST_CASE("config: mac selector value must be a parseable MAC address") {
    auto cam_with_selector = [](const std::string &by, const std::string &value) {
        return "version: 1\ncameras:\n  - id: cam0\n    selector: {by: " + by +
               ", value: \"" + value + "\"}\n";
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
    CHECK(contains(error_of("version: 1\ncameras:\n  - id: cam0\n    selector: {}\n"),
        "selector requires \"by\" and \"value\""));
}

TEST_CASE("config: record_align must be a power of two") {
    CHECK(contains(error_of(doc_with_top("recording: {record_align: 4095}")), "power of two"));
    CHECK(contains(error_of(doc_with_top("recording: {record_align: 0}")), "power of two"));
    CHECK(parse_ok(doc_with_top("recording: {record_align: 1}")).recording.record_align == 1u);
    CHECK(parse_ok(doc_with_top("recording: {record_align: 512}")).recording.record_align == 512u);
}
