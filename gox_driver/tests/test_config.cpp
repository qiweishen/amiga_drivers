// Tests for the lenient YAML config parser (app_config.cpp, fx10-aligned
// schema): defaults for absent keys, unknown keys ignored, and the
// critical-invariant validations.

#include "../include/app_config.hpp"

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
            "    device: {ip: 10.0.0.2}\n";

    // Minimal valid document with extra top-level YAML spliced in.
    std::string doc_with_top(const std::string &extra) {
        return extra + "\n" + kMinimalCameras;
    }

    // Minimal valid document with extra keys spliced into cameras[0] (must be
    // indented 4 spaces to sit inside the camera map).
    std::string doc_with_camera(const std::string &extra) {
        return kMinimalCameras + extra + "\n";
    }
} // namespace

TEST_CASE("config: minimal valid config fills every default") {
    const jai::AppConfig cfg = parse_ok(kMinimalCameras);
    CHECK(cfg.stats_interval_s == doctest::Approx(5.0));
    CHECK(cfg.output.output_dir == "/data/captures");
    CHECK(cfg.output.segment_size_gib == doctest::Approx(2.0));
    CHECK(cfg.output.record_align == 4096u);
    CHECK(cfg.output.queue_max_frames == 32u);
    CHECK(cfg.output.queue_on_full == "drop_newest");
    CHECK(cfg.output.on_buffer_error == "record_flagged");
    CHECK(cfg.output.flush_interval_mb == 64u);
    CHECK(cfg.output.max_frames == 0u);
    CHECK(cfg.output.max_duration_s == doctest::Approx(0.0));
    CHECK(cfg.disk.min_free_gb == doctest::Approx(10.0));
    CHECK(cfg.watchdog.no_frame_warn_s == doctest::Approx(5.0));
    CHECK(cfg.watchdog.no_frame_abort_s == doctest::Approx(-1.0));
    CHECK_FALSE(cfg.ptp.enabled); // PTP is opt-in
    CHECK(cfg.ptp.feature_set == "auto");
    CHECK(cfg.ptp.required_status == "Slave");
    CHECK(cfg.ptp.on_timeout == "abort");

    REQUIRE(cfg.cameras.size() == 1u);
    const jai::CameraConfig &cam = cfg.cameras[0];
    CHECK(cam.id == "cam0");
    CHECK(cam.enabled);
    CHECK(cam.device.mac.empty());
    CHECK(cam.device.ip == "10.0.0.2");
    CHECK_FALSE(cam.device.force_ip.enabled);
    CHECK_FALSE(cam.acquisition.exposure_ms.has_value());
    CHECK_FALSE(cam.acquisition.roi.has_value());
    CHECK(cam.acquisition.trigger.mode == "freerun");
    CHECK(cam.acquisition.trigger.activation == "rising");
    CHECK(cam.acquisition.trigger.selector_entry == "FrameStart");
    CHECK(cam.acquisition.trigger.source_entry == "Line1");
    CHECK(cam.features.raw.empty());
    CHECK(cam.network.channel == 0u);
    CHECK(cam.network.buffer_count == 0u);
    CHECK(cam.network.socket_rx_buffer_mb == 16u);
}

TEST_CASE("config: unknown keys are ignored") {
    const std::string text =
            "bogus_top_level: 42\n"
            "logging: {stats_intervall_s: 99}\n" // typo key: ignored
            "cameras:\n"
            "  - id: cam0\n"
            "    device: {ip: 10.0.0.2}\n"
            "    netwrok: {buffer_count: 2}\n"; // typo key: ignored, no range check
    const jai::AppConfig cfg = parse_ok(text);
    CHECK(cfg.stats_interval_s == doctest::Approx(5.0)); // typo left the default
    CHECK(cfg.cameras[0].network.buffer_count == 0u);
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
        {doc_with_top("output: {segment_size_gib: 0}"), "output.segment_size_gib must be > 0"},
        {doc_with_top("output: {queue_max_frames: 1}"), "must be >= 2"},
        {doc_with_top("output: {flush_interval_mb: 0}"), "output.flush_interval_mb must be > 0"},
        {doc_with_top("output: {record_align: 4095}"), "power of two"},
        {doc_with_top("output: {record_align: 0}"), "power of two"},
        {doc_with_camera("    network: {buffer_count: 2}"), "must be 0 (auto) or >= 4"},
        {doc_with_camera("    network: {packet_size: 100}"), "in [576, 16000]"},
        {doc_with_camera("    network: {socket_rx_buffer_mb: 0}"), "must be > 0"},
        {
            "cameras:\n  - id: bad/name\n    device: {ip: 10.0.0.2}\n",
            "only [A-Za-z0-9_-]"
        },
        {"cameras:\n  - device: {ip: 10.0.0.2}\n", "id is required"},
        {"cameras:\n  - id: cam0\n", "device requires mac or ip"},
    };
    for (const Case &c: cases) {
        CAPTURE(c.yaml);
        CHECK(contains(error_of(c.yaml), c.expect));
    }
}

TEST_CASE("config: camera list validation") {
    const std::string dup =
            "cameras:\n"
            "  - {id: cam0, device: {ip: 10.0.0.2}}\n"
            "  - {id: cam0, device: {ip: 10.0.0.3}}\n";
    CHECK(contains(error_of(dup), "duplicate camera id \"cam0\""));

    CHECK(contains(error_of("cameras: []\n"), "non-empty list"));
    CHECK(contains(error_of("output: {}\n"), "non-empty list"));
    CHECK(contains(error_of(""), "empty document"));

    const std::string all_disabled =
            "cameras:\n"
            "  - {id: cam0, enabled: false, device: {ip: 10.0.0.2}}\n"
            "  - {id: cam1, enabled: false, device: {ip: 10.0.0.3}}\n";
    CHECK(contains(error_of(all_disabled), "at least one camera must be enabled"));

    // One disabled + one enabled camera is a valid multi-camera config.
    const std::string mixed =
            "cameras:\n"
            "  - {id: cam0, enabled: false, device: {ip: 10.0.0.2}}\n"
            "  - {id: cam1, device: {ip: 10.0.0.3}}\n";
    const jai::AppConfig cfg = parse_ok(mixed);
    REQUIRE(cfg.cameras.size() == 2u);
    CHECK_FALSE(cfg.cameras[0].enabled);
    CHECK(cfg.cameras[1].enabled);
}

TEST_CASE("config: device selection is validated (fx10 semantics)") {
    // All common MAC spellings are accepted verbatim (normalization happens at
    // discovery-match time).
    CHECK(parse_ok("cameras:\n  - id: c\n    device: {mac: \"00:0C:DF:12:34:56\"}\n")
          .cameras[0].device.mac == "00:0C:DF:12:34:56");
    CHECK(parse_ok("cameras:\n  - id: c\n    device: {mac: \"000cdf123456\"}\n")
          .cameras[0].device.mac == "000cdf123456");
    CHECK(contains(error_of("cameras:\n  - id: c\n    device: {mac: \"00:0c:df:12:34\"}\n"),
        "not a valid MAC address"));

    // force_ip needs a MAC (FORCEIP addresses the camera by MAC) and both addresses.
    CHECK(contains(error_of(
              "cameras:\n  - id: c\n    device:\n      ip: 10.0.0.2\n"
              "      force_ip: {enabled: true, ip: 10.0.0.9, subnet_mask: 255.255.255.0}\n"),
        "requires device.mac"));
    CHECK(contains(error_of(
              "cameras:\n  - id: c\n    device:\n      mac: \"00:0c:df:12:34:56\"\n"
              "      force_ip: {enabled: true}\n"),
        "requires ip and subnet_mask"));
}

TEST_CASE("config: trigger enums follow the fx10 vocabulary") {
    const jai::AppConfig cfg = parse_ok(doc_with_camera(
        "    acquisition:\n"
        "      trigger: {mode: external, activation: falling, source_entry: Line2}\n"));
    CHECK(cfg.cameras[0].acquisition.trigger.mode == "external");
    CHECK(cfg.cameras[0].acquisition.trigger.activation == "falling");
    CHECK(cfg.cameras[0].acquisition.trigger.source_entry == "Line2");

    CHECK(contains(error_of(doc_with_camera("    acquisition: {trigger: {mode: sometimes}}")),
        "acquisition.trigger.mode"));
    CHECK(contains(error_of(doc_with_camera("    acquisition: {trigger: {activation: up}}")),
        "acquisition.trigger.activation"));
}

TEST_CASE("config: features.raw scalar typing follows YAML quoting") {
    const std::string text = doc_with_camera(
        "    features:\n"
        "      raw:\n"
        "        - {name: Width, value: 1936}\n"
        "        - {name: AcquisitionFrameRate, value: 20.5}\n"
        "        - {name: ReverseX, value: true}\n"
        "        - {name: UserSetLoad, value: ~}\n"
        "        - {name: PixelFormat, value: \"BayerRG10p\"}\n"
        "        - {name: UserSetSave}\n");
    const jai::AppConfig cfg = parse_ok(text);
    const std::vector<jai::RawFeature> &f = cfg.cameras[0].features.raw;
    REQUIRE(f.size() == 6u);
    CHECK(f[0].name == "Width");
    CHECK(f[0].value == "1936"); // plain scalar -> typed by the camera node type
    CHECK_FALSE(f[0].value_is_string);
    CHECK(f[1].value == "20.5");
    CHECK(f[2].value == "true");
    CHECK(f[3].value.empty()); // null -> "" (command features take no value)
    CHECK(f[4].value == "BayerRG10p"); // quoted -> apply as string/enum entry
    CHECK(f[4].value_is_string);
    CHECK(f[5].value.empty()); // absent value: also a command feature

    CHECK(contains(error_of(doc_with_camera("    features: {raw: [{value: 1}]}")),
        "non-empty \"name\""));
}

TEST_CASE("config: enum values are validated and the error lists what is allowed") {
    const std::string err = error_of(doc_with_top("output: {queue_on_full: drop_newst}"));
    CHECK(contains(err, "output.queue_on_full"));
    CHECK(contains(err, "invalid value \"drop_newst\""));
    CHECK(contains(err, "drop_newest"));
    CHECK(contains(err, "block"));

    CHECK(contains(error_of(doc_with_top("output: {on_buffer_error: ignore}")),
        "output.on_buffer_error"));
    CHECK(contains(error_of(doc_with_top("ptp: {feature_set: bogus}")), "ptp.feature_set"));
    CHECK(contains(error_of(doc_with_top("ptp: {on_timeout: retry}")), "ptp.on_timeout"));
}

TEST_CASE("config: watchdog auto resolution follows the fx10 rule") {
    const jai::AppConfig cfg = parse_ok(kMinimalCameras);
    CHECK(cfg.watchdog.resolved_no_frame_abort_s("external") == doctest::Approx(0.0)); // silence = pulses stopped
    CHECK(cfg.watchdog.resolved_no_frame_abort_s("freerun") == doctest::Approx(10.0)); // auto in freerun

    const jai::AppConfig explicit_abort = parse_ok(doc_with_top("watchdog: {no_frame_abort_s: 30}"));
    CHECK(explicit_abort.watchdog.resolved_no_frame_abort_s("external") == doctest::Approx(30.0));
    CHECK(explicit_abort.watchdog.resolved_no_frame_abort_s("freerun") == doctest::Approx(30.0));

    const jai::AppConfig warn = parse_ok(doc_with_top("watchdog: {no_frame_warn_s: 2}"));
    CHECK(warn.watchdog.no_frame_warn_s == doctest::Approx(2.0));
}
