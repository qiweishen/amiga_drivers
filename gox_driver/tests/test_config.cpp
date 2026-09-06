// app_config.cpp: strict schema (unknown keys are errors, absent keys keep the
// defaults), every invariant, and the enum vocabulary.

#include "app_config.h"

#include <doctest/doctest.h>

#include <string>

using namespace gox;

namespace {
    AppConfig ParseOk(const std::string &text) { return LoadAppConfigText(text); }

    // "" = parsed
    std::string ErrorOf(const std::string &text) {
        try {
            LoadAppConfigText(text);
        } catch (const ConfigError &e) {
            return e.what();
        }
        return {};
    }

    bool Contains(const std::string &s, const std::string &needle) { return s.find(needle) != std::string::npos; }

    const std::string kMinimalCameras =
            "cameras:\n"
            "  - id: cam0\n"
            "    device: {ip: 10.0.0.2}\n";

    std::string DocWithTop(const std::string &extra) { return extra + "\n" + kMinimalCameras; }

    // extra must be indented 4 spaces to sit inside cameras[0]
    std::string DocWithCamera(const std::string &extra) { return kMinimalCameras + extra + "\n"; }
} // namespace

TEST_CASE("config: minimal valid config fills every default") {
    const AppConfig cfg = ParseOk(kMinimalCameras);
    CHECK(cfg.stats_interval_s == doctest::Approx(2.5));
    CHECK(cfg.output.segment_size_mb == 2048u);
    CHECK(cfg.output.record_align == 4096u);
    CHECK(cfg.output.queue_max_frames == 0u);
    CHECK(cfg.output.queue_on_full == QueueOnFull::kDropNewest);
    CHECK(cfg.output.on_buffer_error == OnBufferError::kRecordFlagged);
    CHECK(cfg.output.flush_interval_mb == 64u);
    CHECK(cfg.output.max_frames == 0u);
    CHECK(cfg.output.max_duration_s == doctest::Approx(0.0));
    CHECK_FALSE(cfg.ptp.enabled);
    CHECK(cfg.ptp.on_timeout == PtpOnTimeout::kAbort);
    CHECK(cfg.ptp.sync_timeout_s == doctest::Approx(60.0));
    CHECK(cfg.ptp.offset_report_interval_s == doctest::Approx(60.0));

    REQUIRE(cfg.cameras.size() == 1u);
    const CameraConfig &cam = cfg.cameras[0];
    CHECK(cam.id == "cam0");
    CHECK(cam.enabled);
    CHECK(cam.device.mac.empty());
    CHECK(cam.device.ip == "10.0.0.2");
    CHECK_FALSE(cam.acquisition.exposure_ms.has_value());
    CHECK_FALSE(cam.acquisition.roi.has_value());
    CHECK_FALSE(cam.acquisition.blemish_correction); // rawest state
    CHECK(cam.acquisition.trigger.mode == TriggerMode::kFreerun);
    CHECK(cam.acquisition.trigger.activation == TriggerActivation::kRising);
    CHECK(cam.acquisition.trigger.selector_entry == "FrameStart");
    CHECK(cam.acquisition.trigger.source_entry == "24"); // Line5 Opt In; a GO-X has no "Line1"
    CHECK(cam.features.raw.empty());
    CHECK(cam.network.buffer_count == 0u);
    CHECK(cam.network.socket_rx_buffer_mb == 16u);
    CHECK(cam.network.throughput_safety_margin_pct == 0u);
}

TEST_CASE("config: unknown keys are errors that name the key and the accepted set") {
    const std::string top = ErrorOf(DocWithTop("bogus_top_level: 42"));
    CHECK(Contains(top, "unknown key 'bogus_top_level'"));
    CHECK(Contains(top, "accepts only: cameras, output, ptp, logging"));

    CHECK(Contains(ErrorOf(DocWithTop("logging: {stats_intervall_s: 99}")), "unknown key 'logging.stats_intervall_s'"));
    CHECK(Contains(ErrorOf(DocWithCamera("    netwrok: {buffer_count: 2}")), "unknown key 'cameras[0].netwrok'"));
    CHECK(Contains(ErrorOf(DocWithCamera("    network: {channel: 0}")), "unknown key 'cameras[0].network.channel'"));
    CHECK(Contains(ErrorOf(DocWithTop("ptp: {enabled: true, feature_set: sfnc_ptp}")), "unknown key 'ptp.feature_set'"));
    // Retired keys are errors too: silently ignoring them is how templates rot
    CHECK(Contains(ErrorOf(DocWithTop("output: {output_dir: /tmp}")), "unknown key 'output.output_dir'"));
    CHECK(Contains(ErrorOf(DocWithTop("output: {file_prefix: gox}")), "unknown key 'output.file_prefix'"));
    CHECK(Contains(ErrorOf(DocWithTop("output: {segment_size_gib: 2}")), "unknown key 'output.segment_size_gib'"));
    CHECK(Contains(ErrorOf("cameras:\n  - id: c\n    device:\n      ip: 10.0.0.2\n"
                           "      force_ip: {enabled: true}\n"),
                   "unknown key 'cameras[0].device.force_ip'"));
    CHECK(Contains(ErrorOf(DocWithCamera("    features: {raw: [{name: Width, value: 8, type: int}]}")),
                   "unknown key 'cameras[0].features.raw[0].type'"));
}

TEST_CASE("config: invariants are rejected with the dotted key") {
    struct Case {
        std::string yaml;
        const char *expect;
    };
    const Case cases[] = {
        {DocWithTop("logging: {stats_interval_s: 0}"), "logging.stats_interval_s: must be > 0"},
        {DocWithTop("output: {segment_size_mb: 0}"), "output.segment_size_mb: must be in [1, 1048576]"},
        {DocWithTop("output: {queue_max_frames: 1}"), "output.queue_max_frames: must be >= 2"},
        {DocWithTop("output: {flush_interval_mb: 0}"), "output.flush_interval_mb: must be in"},
        {DocWithTop("output: {record_align: 4095}"), "output.record_align: must be a power of two"},
        {DocWithTop("output: {record_align: 0}"), "output.record_align"},
        {DocWithTop("output: {max_frames: -1}"), "output.max_frames: must not be negative"},
        {DocWithTop("output: {max_duration_s: -5}"), "output.max_duration_s: must be in [0"},
        {DocWithTop("ptp: {sync_timeout_s: 0}"), "ptp.sync_timeout_s: must be > 0"},
        {DocWithTop("ptp: {offset_report_interval_s: -1}"), "ptp.offset_report_interval_s"},
        {DocWithCamera("    network: {buffer_count: 2}"), "cameras[0].network.buffer_count: must be 0 (auto) or >= 4"},
        {DocWithCamera("    network: {buffer_count: -1}"), "cameras[0].network.buffer_count: must not be negative"},
        {DocWithCamera("    network: {packet_size: 100}"), "in [1476, 12036]"},
        {DocWithCamera("    network: {packet_size: 20000}"), "in [1476, 12036]"},
        {DocWithCamera("    network: {socket_rx_buffer_mb: 0}"), "cameras[0].network.socket_rx_buffer_mb: must be in [1, 1024]"},
        {DocWithCamera("    network: {socket_rx_buffer_mb: 4096}"), "must be in [1, 1024]"},
        {DocWithCamera("    network: {throughput_safety_margin_pct: 5}"), "in [10, 100]"},
        {DocWithCamera("    network: {throughput_safety_margin_pct: 120}"), "in [10, 100]"},
        {DocWithCamera("    acquisition: {exposure_ms: 0}"), "cameras[0].acquisition.exposure_ms: must be > 0"},
        {DocWithCamera("    acquisition: {gain: 0.5}"), "gain: must be in [1.0, 126.0]"},
        {DocWithCamera("    acquisition: {gain: 200}"), "gain: must be in [1.0, 126.0]"},
        {DocWithCamera("    acquisition: {frame_rate_hz: 0.1}"), "frame_rate_hz: must be >= 0.125"},
        {DocWithCamera("    acquisition: {roi: {width: 100}}"), "roi.width: must be a multiple of 8"},
        {DocWithCamera("    acquisition: {roi: {width: 64}}"), "must be 0 (keep) or >= 96"},
        {DocWithCamera("    acquisition: {roi: {height: 4}}"), "must be 0 (keep) or >= 8"},
        {DocWithCamera("    acquisition: {roi: {height: 101}}"), "multiple of 2"},
        {DocWithCamera("    acquisition: {roi: {offset_x: 4}}"), "roi.offset_x: must be a multiple of 8"},
        {DocWithCamera("    acquisition: {roi: {offset_y: 3}}"), "roi.offset_y: must be a multiple of 2"},
        {DocWithCamera("    acquisition: {blemish_correction: maybe}"), "cameras[0].acquisition.blemish_correction: has an invalid value"},
        {DocWithCamera("    acquisition: {exposure_ms:}"), "cameras[0].acquisition.exposure_ms: has an invalid value"},
        {"cameras:\n  - id: bad/name\n    device: {ip: 10.0.0.2}\n", "cameras[0].id: only [A-Za-z0-9_-]"},
        {"cameras:\n  - device: {ip: 10.0.0.2}\n", "cameras[0].id: is required"},
        {"cameras:\n  - id: cam0\n", "cameras[0].device: requires mac or ip"},
    };
    for (const Case &c: cases) {
        CAPTURE(c.yaml);
        CHECK(Contains(ErrorOf(c.yaml), c.expect));
    }
}

TEST_CASE("config: camera list validation") {
    const std::string dup =
            "cameras:\n"
            "  - {id: cam0, device: {ip: 10.0.0.2}}\n"
            "  - {id: cam0, device: {ip: 10.0.0.3}}\n";
    CHECK(Contains(ErrorOf(dup), "duplicate camera id \"cam0\""));

    CHECK(Contains(ErrorOf("cameras: []\n"), "cameras: must be a non-empty list"));
    CHECK(Contains(ErrorOf("output: {}\n"), "cameras: is required"));
    CHECK(Contains(ErrorOf(""), "cameras: is required"));
    CHECK(Contains(ErrorOf("cameras:\n"), "cameras: must be a list"));

    const std::string all_disabled =
            "cameras:\n"
            "  - {id: cam0, enabled: false, device: {ip: 10.0.0.2}}\n"
            "  - {id: cam1, enabled: false, device: {ip: 10.0.0.3}}\n";
    CHECK(Contains(ErrorOf(all_disabled), "at least one camera must be enabled"));

    const std::string mixed =
            "cameras:\n"
            "  - {id: cam0, enabled: false, device: {ip: 10.0.0.2}}\n"
            "  - {id: cam1, device: {ip: 10.0.0.3}}\n";
    const AppConfig cfg = ParseOk(mixed);
    REQUIRE(cfg.cameras.size() == 2u);
    CHECK_FALSE(cfg.cameras[0].enabled);
    CHECK(cfg.cameras[1].enabled);
}

TEST_CASE("config: device selection") {
    CHECK(ParseOk("cameras:\n  - id: c\n    device: {mac: \"00:0C:DF:12:34:56\"}\n").cameras[0].device.mac ==
          "00:0C:DF:12:34:56");
    CHECK(ParseOk("cameras:\n  - id: c\n    device: {mac: \"000cdf123456\"}\n").cameras[0].device.mac == "000cdf123456");
    CHECK(Contains(ErrorOf("cameras:\n  - id: c\n    device: {mac: \"00:0c:df:12:34\"}\n"),
                   "cameras[0].device.mac: is not a valid MAC address"));
}

TEST_CASE("config: trigger enums") {
    const AppConfig cfg = ParseOk(DocWithCamera(
        "    acquisition:\n"
        "      trigger: {mode: external, activation: falling, source_entry: 19}\n"));
    CHECK(cfg.cameras[0].acquisition.trigger.mode == TriggerMode::kExternal);
    CHECK(cfg.cameras[0].acquisition.trigger.activation == TriggerActivation::kFalling);
    CHECK(cfg.cameras[0].acquisition.trigger.source_entry == "19");

    CHECK(Contains(ErrorOf(DocWithCamera("    acquisition: {trigger: {mode: sometimes}}")),
                   "cameras[0].acquisition.trigger.mode: invalid value 'sometimes'; allowed: freerun | external"));
    CHECK(Contains(ErrorOf(DocWithCamera("    acquisition: {trigger: {activation: up}}")),
                   "allowed: rising | falling"));
    CHECK(Contains(ErrorOf(DocWithCamera("    acquisition: {trigger: {selector_entry: \"\"}}")),
                   "selector_entry: must not be blank"));
}

TEST_CASE("config: features.raw scalar typing follows YAML quoting") {
    const std::string text = DocWithCamera(
        "    features:\n"
        "      raw:\n"
        "        - {name: Width, value: 1936}\n"
        "        - {name: AcquisitionFrameRate, value: 20.5}\n"
        "        - {name: ReverseX, value: true}\n"
        "        - {name: UserSetLoad, value: ~}\n"
        "        - {name: PixelFormat, value: \"BayerRG10p\"}\n"
        "        - {name: UserSetSave}\n");
    const AppConfig cfg = ParseOk(text);
    const std::vector<RawFeature> &f = cfg.cameras[0].features.raw;
    REQUIRE(f.size() == 6u);
    CHECK(f[0].name == "Width");
    CHECK(f[0].value == "1936");
    CHECK_FALSE(f[0].value_is_string);
    CHECK(f[1].value == "20.5");
    CHECK(f[2].value == "true");
    CHECK(f[3].value.empty()); // null -> command feature
    CHECK(f[4].value == "BayerRG10p"); // quoted -> enum entry / string
    CHECK(f[4].value_is_string);
    CHECK(f[5].value.empty());

    CHECK(Contains(ErrorOf(DocWithCamera("    features: {raw: [{value: 1}]}")), "cameras[0].features.raw[0].name: is required"));
    CHECK(Contains(ErrorOf(DocWithCamera("    features: {raw: 5}")), "cameras[0].features.raw: must be a list"));
    CHECK(ParseOk(DocWithCamera("    network: {receiver_tuning: [{name: RequestTimeout, value: 5000}]}"))
          .cameras[0].network.receiver_tuning.size() == 1u);
}

TEST_CASE("config: output/ptp enums list what is allowed") {
    const std::string err = ErrorOf(DocWithTop("output: {queue_on_full: drop_newst}"));
    CHECK(Contains(err, "output.queue_on_full: invalid value 'drop_newst'; allowed: drop_newest | block"));
    CHECK(Contains(ErrorOf(DocWithTop("output: {on_buffer_error: ignore}")), "allowed: record_flagged | drop"));
    CHECK(Contains(ErrorOf(DocWithTop("ptp: {on_timeout: retry}")), "allowed: abort | warn_continue"));
    CHECK(ParseOk(DocWithTop("output: {queue_on_full: block, on_buffer_error: drop}\nptp: {on_timeout: warn_continue}"))
          .ptp.on_timeout == PtpOnTimeout::kWarnContinue);
}

TEST_CASE("config: the trigger source is checked against the GO-X's own list") {
    for (const char *ok: {"24", "19", "7", "10", "11", "14", "15", "18", "36", "37"}) {
        CAPTURE(ok);
        CHECK(ErrorOf(DocWithCamera(std::string("    acquisition: {trigger: {source_entry: ") + ok + "}}")).empty());
    }
    for (const char *bad: {"1", "25", "0", "99", "99999999999"}) {
        CAPTURE(bad);
        CHECK(Contains(ErrorOf(DocWithCamera(std::string("    acquisition: {trigger: {source_entry: ") + bad + "}}")),
                       "is not a GO-X TriggerSource value"));
    }
    CHECK(ParseOk(DocWithCamera("    acquisition: {trigger: {source_entry: Software}}"))
          .cameras[0].acquisition.trigger.source_entry == "Software");
}

TEST_CASE("config: a freerun exposure must fit inside the frame period") {
    CHECK(Contains(ErrorOf(DocWithCamera("    acquisition: {exposure_ms: 400, frame_rate_hz: 3.0}")),
                   "must be below the freerun frame period"));
    CHECK(Contains(ErrorOf(DocWithCamera("    acquisition: {exposure_ms: 333.4, frame_rate_hz: 3.0}")),
                   "must be below the freerun frame period"));
    CHECK(ErrorOf(DocWithCamera("    acquisition: {exposure_ms: 150, frame_rate_hz: 3.0}")).empty());
    // Under an external trigger the pulse train sets the period
    CHECK(ErrorOf(DocWithCamera(
        "    acquisition:\n"
        "      exposure_ms: 400\n"
        "      frame_rate_hz: 3.0\n"
        "      trigger: {mode: external}")).empty());
    CHECK(ErrorOf(DocWithCamera("    acquisition: {exposure_ms: 4000}")).empty());
}
