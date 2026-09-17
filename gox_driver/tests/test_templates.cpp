// The shipped templates must parse and mean what they say.

#include "app_config.h"

#include <doctest/doctest.h>

#include <string>

#ifndef GOX_CONFIG_DIR
#error "GOX_CONFIG_DIR must be defined by the build (see gox_driver/CMakeLists.txt)"
#endif

using namespace gox;

namespace {
    std::string ConfigPath(const char *name) { return std::string(GOX_CONFIG_DIR) + "/" + name; }
} // namespace

TEST_CASE("templates: config-gox.yaml parses and keeps its documented values") {
    const AppConfig cfg = LoadAppConfig(ConfigPath("config-gox.yaml"));

    REQUIRE(cfg.cameras.size() == 1u);
    const CameraConfig &cam = cfg.cameras[0];
    CHECK(cam.id == "cam0");
    CHECK(cam.enabled);
    CHECK(cam.device.mac.empty());
    CHECK_FALSE(cam.device.ip.empty());

    REQUIRE(cam.acquisition.pixel_format.has_value());
    CHECK(*cam.acquisition.pixel_format == "BayerRG12Packed");
    CHECK_FALSE(cam.acquisition.blemish_correction);
    REQUIRE(cam.acquisition.frame_rate_hz.has_value());
    CHECK(*cam.acquisition.frame_rate_hz == doctest::Approx(5.0));
    REQUIRE(cam.acquisition.exposure_ms.has_value());
    CHECK(*cam.acquisition.exposure_ms == doctest::Approx(150.0));
    REQUIRE(cam.acquisition.gain.has_value());
    CHECK(*cam.acquisition.gain == doctest::Approx(1.0));
    CHECK(*cam.acquisition.exposure_ms < 900.0 / *cam.acquisition.frame_rate_hz);

    // The rig triggers the Go-X from the SensorSync board: Line5 Opt In takes the
    // pulses, Line2 Opt Out returns ExposureActive, and the board is the FX10's.
    CHECK(cam.acquisition.trigger.mode == TriggerMode::kExternal);
    CHECK(cam.acquisition.trigger.activation == TriggerActivation::kRising);
    CHECK(cam.acquisition.trigger.selector_entry == "FrameStart");
    CHECK(cam.acquisition.trigger.source_entry == "24");
    CHECK(cam.acquisition.trigger.exposure_active_output);
    CHECK(cam.acquisition.trigger.sensor_channel == 2);
    CHECK(cfg.sensor_trigger.enabled); // the board's port lives in config-main.yaml
    CHECK(*cam.acquisition.frame_rate_hz >= 1.0); // the SensorSync JAI pair runs 1..10 Hz
    CHECK(*cam.acquisition.frame_rate_hz <= 10.0);

    REQUIRE(cam.acquisition.roi.has_value());
    CHECK(cam.acquisition.roi->width == 0u);
    CHECK(cam.acquisition.roi->height == 0u);

    CHECK(cam.features.raw.empty());

    CHECK(cam.network.buffer_count == 0u);
    CHECK(cam.network.packet_size == 0u);
    CHECK(cam.network.socket_rx_buffer_mb == 16u);
    CHECK(cam.network.throughput_safety_margin_pct == 0u);
    CHECK(cam.network.receiver_tuning.empty());

    CHECK(cfg.output.segment_size_mb == 2048u);
    CHECK(cfg.output.queue_max_frames == 0u);
    CHECK(cfg.output.queue_on_full == QueueOnFull::kDropNewest);
    CHECK(cfg.output.on_buffer_error == OnBufferError::kRecordFlagged);
    CHECK(cfg.output.record_align == 4096u);
    CHECK(cfg.output.max_frames == 0u);
    CHECK(cfg.output.max_duration_s == doctest::Approx(0.0));

    CHECK(cfg.stats_interval_s == doctest::Approx(2.5));

    // Current capture profile explicitly disables PTP. Omitted-key defaults
    // and enabled PTP parsing are covered separately in test_config.cpp.
    CHECK_FALSE(cfg.ptp.enabled);
    CHECK(cfg.ptp.on_timeout == PtpOnTimeout::kAbort);
}

TEST_CASE("templates: config-gox-snapshot.yaml keeps the single-shot invariants") {
    const AppConfig cfg = LoadAppConfig(ConfigPath("config-gox-snapshot.yaml"));

    REQUIRE(cfg.cameras.size() == 1u);
    const CameraConfig &cam = cfg.cameras[0];
    CHECK(cam.id == "cam0");
    REQUIRE(cam.acquisition.pixel_format.has_value());
    CHECK(*cam.acquisition.pixel_format == "BayerRG12Packed"); // must be in unpack_raw.py's PFNC table
    // Absent on purpose: the default must match what config-gox.yaml records with
    CHECK_FALSE(cam.acquisition.blemish_correction);

    // Without a frame rate the camera stays at its factory 8 fps and clamps the GUI's exposure range
    REQUIRE(cam.acquisition.frame_rate_hz.has_value());
    CHECK(*cam.acquisition.frame_rate_hz == doctest::Approx(3.0));
    REQUIRE(cam.acquisition.exposure_ms.has_value());
    CHECK(*cam.acquisition.exposure_ms < 900.0 / *cam.acquisition.frame_rate_hz);

    // jai_snapshot forces max_frames / max_duration_s / ptp / trigger itself; the template stays silent
    CHECK(cfg.output.max_frames == 0u);
    CHECK(cfg.output.max_duration_s == doctest::Approx(0.0));
    CHECK_FALSE(cfg.ptp.enabled);
    CHECK(cam.acquisition.trigger.mode == TriggerMode::kFreerun);

    CHECK(cfg.output.queue_on_full == QueueOnFull::kBlock); // the single frame must never be dropped
    CHECK(cfg.output.record_align == 4096u); // snapshot_main sizes the empty segment from this
    CHECK(cam.network.buffer_count >= 4u);
}

TEST_CASE("templates: a missing file reports the path") {
    CHECK_THROWS_AS(LoadAppConfig(ConfigPath("config-does-not-exist.yaml")), ConfigError);
}
