// app_config.cpp: strict schema, defaults, every invariant, the shipped templates.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <utility>

#include "app_config.h"

using namespace fx10;

namespace {
    std::string ErrorOf(const std::string &yaml) {
        try {
            LoadAppConfigText(yaml);
        } catch (const ConfigError &e) {
            return e.what();
        }
        return {};
    }

    bool Contains(const std::string &s, const std::string &needle) { return s.find(needle) != std::string::npos; }
} // namespace

TEST_CASE("config: an empty document yields the defaults") {
    const AppConfig c = LoadAppConfigText("");
    CHECK(c.acquisition.pixel_format == "Mono12Packed");
    CHECK(c.acquisition.spectral_binning == 2); // factory (manual p.44)
    CHECK(c.acquisition.spatial_binning == 1);
    CHECK(c.acquisition.trigger.mode == TriggerMode::kExternal);
    CHECK(c.acquisition.trigger.source_entry == "Line0");
    CHECK(c.device.mac.empty());
    CHECK_FALSE(c.sensor_trigger.enabled);
    CHECK(c.sensor_trigger.trigger_channel == 0);
    CHECK(c.network.buffer_count == 0);
    CHECK(c.recording.output_dir.empty()); // injected by the host
    CHECK(c.recording.on_gap == GapPolicy::kRecord);
    CHECK(c.recording.rotation.max_lines == 0u);
    CHECK(c.recording.rotation.max_mb == 2048u);
    CHECK(c.recording.flush_interval_mb == 64u);
    CHECK(c.recording.wavelengths.source == WavelengthSource::kNone);
    CHECK(c.features.raw.empty());
    CHECK(c.logging.stats_interval_s == doctest::Approx(2.5));
}

TEST_CASE("config: a full document round-trips") {
    const AppConfig c = LoadAppConfigText(R"(
device:
  mac: "00:0C:DF:12:34:56"
  ip: "10.0.0.5"
network:
  packet_size: 9000
  socket_rx_buffer_mb: 64
  buffer_count: 128
  stall_budget_s: 1.5
  max_buffer_memory_mb: 256
  retrieve_timeout_ms: 500
acquisition:
  pixel_format: Mono8
  spatial_binning: 2
  spectral_binning: 4
  exposure_ms: 2.5
  frame_rate_hz: 100
  trigger: { mode: freerun, activation: falling, delay_ms: 1.5, exposure_control: pulse_width }
sensor_trigger:
  enabled: true
  trigger_channel: 2
output:
  rotation: { max_lines: 5000, max_mb: 1024 }
  flush_interval_mb: 16
  on_gap: pad_zero
  wavelengths:
    source: list
    list: [400.5, 500.5, 600.5]
    fwhm_list: [5.5, 5.5, 5.5]
  max_duration_s: 120.5
  max_frames: 9000
features:
  raw:
    - { name: TestPattern, type: enum, value: "Off" }
    - { name: DeviceReset, type: command }
logging:
  stats_interval_s: 10
)");
    CHECK(c.device.mac == "00:0C:DF:12:34:56");
    CHECK(c.device.ip == "10.0.0.5");
    CHECK(c.network.packet_size == 9000);
    CHECK(c.network.socket_rx_buffer_mb == 64);
    CHECK(c.network.buffer_count == 128);
    CHECK(c.network.stall_budget_s == doctest::Approx(1.5));
    CHECK(c.acquisition.pixel_format == "Mono8");
    CHECK(c.acquisition.spectral_binning == 4);
    CHECK(c.acquisition.spatial_binning == 2);
    CHECK(c.acquisition.trigger.mode == TriggerMode::kFreerun);
    CHECK(c.acquisition.trigger.activation == TriggerActivation::kFalling);
    CHECK(c.acquisition.trigger.delay_ms == doctest::Approx(1.5));
    CHECK(c.acquisition.trigger.exposure_control == ExposureControl::kPulseWidth);
    CHECK(c.sensor_trigger.enabled);
    CHECK(c.sensor_trigger.trigger_channel == 2);
    CHECK(c.recording.rotation.max_lines == 5000u);
    CHECK(c.recording.rotation.max_mb == 1024u);
    CHECK(c.recording.flush_interval_mb == 16u);
    CHECK(c.recording.on_gap == GapPolicy::kPadZero);
    CHECK(c.recording.max_duration_s == doctest::Approx(120.5));
    CHECK(c.recording.max_frames == 9000u);
    CHECK(c.recording.wavelengths.source == WavelengthSource::kList);
    CHECK(c.recording.wavelengths.list.size() == 3u);
    CHECK(c.recording.wavelengths.fwhm_list.size() == 3u);
    REQUIRE(c.features.raw.size() == 2u);
    CHECK(c.features.raw[0].name == "TestPattern");
    CHECK(c.features.raw[0].value == "Off");
    CHECK(c.features.raw[1].type == "command");
    CHECK(c.logging.stats_interval_s == doctest::Approx(10.0));
}

TEST_CASE("config: the shipped templates parse and stay in sync") {
    const AppConfig prod = LoadAppConfig(std::string(FX10_CONFIG_DIR) + "/config-fx10.yaml");
    CHECK(prod.logging.stats_interval_s == doctest::Approx(2.5));
    CHECK(prod.sensor_trigger.enabled); // the board's port lives in config-main.yaml
    CHECK(prod.recording.on_gap == GapPolicy::kPadZero);

    const AppConfig snap = LoadAppConfig(std::string(FX10_CONFIG_DIR) + "/config-fx10-snapshot.yaml");
    CHECK_FALSE(snap.sensor_trigger.enabled);
    CHECK(snap.features.raw.empty());

    for (const AppConfig *c: {&prod, &snap}) {
        // The sensor window comes from the calibration pack (manual p.25)
        for (const auto &f: c->features.raw) {
            CAPTURE(f.name);
            CHECK((f.name != "OffsetX" && f.name != "OffsetY" && f.name != "Width" && f.name != "Height"));
        }
        CHECK(c->acquisition.spectral_binning == 2); // factory value (manual p.44)
        CHECK(c->acquisition.ExpectedBands() == 224);
        CHECK(c->acquisition.spatial_binning == 1);
        CHECK(c->device.ebus_config == "fx10_driver/resource/ebus_config/077000060381/3210798.pvcfg");
        CHECK(c->recording.flush_interval_mb > 0u);
        // The template cannot prove whether this firmware exposes LineSelector.
        // When both writes are configured, retain their order. CameraControl
        // requires a preceding selector at runtime if the actual node exists.
        const auto source = std::find_if(c->features.raw.begin(), c->features.raw.end(),
                                         [](const RawFeature &f) { return f.name == "LineSource"; });
        const auto selector = std::find_if(c->features.raw.begin(), c->features.raw.end(),
                                           [](const RawFeature &f) { return f.name == "LineSelector"; });
        if (source != c->features.raw.end() && selector != c->features.raw.end()) {
            CHECK(selector < source);
        }
    }
}

TEST_CASE("config: unknown keys are errors naming the key and the accepted set") {
    const std::string top = ErrorOf("bogus: 1\n");
    CHECK(Contains(top, "unknown key 'bogus'"));
    CHECK(Contains(top, "accepts only: device, acquisition, sensor_trigger, features, output, network, logging"));
    CHECK(Contains(ErrorOf("network: { mtu: 9000 }"), "unknown key 'network.mtu'"));
    CHECK(Contains(ErrorOf("device: { ip: \"10.0.0.5\", force_ip: { enabled: true } }"), "unknown key 'device.force_ip'"));
    CHECK(Contains(ErrorOf("device: { id: cam-01 }"), "unknown key 'device.id'"));
    CHECK(Contains(ErrorOf("output: { output_dir: /tmp }"), "unknown key 'output.output_dir'"));
    CHECK(Contains(ErrorOf("output: { file_prefix: x }"), "unknown key 'output.file_prefix'"));
    CHECK(Contains(ErrorOf("output: { rotation: { max_megabytes: 1 } }"), "unknown key 'output.rotation.max_megabytes'"));
    CHECK(Contains(ErrorOf("features: { map: { exposure_time: ExposureTime } }"), "unknown key 'features.map'"));
    CHECK(Contains(ErrorOf("features: { raw: [ { name: A, type: int, value: 1, extra: 2 } ] }"),
                   "unknown key 'features.raw[0].extra'"));
}

TEST_CASE("config: raw writes cannot bypass acquisition ordering or repeat shutter pulses") {
    for (const std::string name: {"AcquisitionStart", "AcquisitionStop"}) {
        CHECK(Contains(ErrorOf("features: { raw: [ { name: " + name + ", type: command } ] }"),
                       "features.raw[0].name"));
    }
    for (const std::string name: {"MotorShutter_PulseRev", "MotorShutter_PulseFwd"}) {
        CHECK(Contains(ErrorOf("features: { raw: [ { name: " + name + ", type: int, value: 255 } ] }"),
                       "features.raw[0].name"));
    }
    // No generic state binding or disable switch: opening is a mandatory action.
    CHECK(Contains(ErrorOf("shutter: { opened: { name: MotorShutter_PulseRev, type: int, value: 255 } }"),
                   "unknown key 'shutter'"));
}

TEST_CASE("config: invalid scalars name the key") {
    CHECK(Contains(ErrorOf("acquisition: { exposure_ms: banana }"), "acquisition.exposure_ms: has an invalid value"));
    CHECK(Contains(ErrorOf("acquisition: { exposure_ms: }"), "acquisition.exposure_ms: has an invalid value"));
    CHECK(Contains(ErrorOf("acquisition: { trigger: { mode: \"\" } }"), "acquisition.trigger.mode: invalid value ''"));
}

TEST_CASE("config: acquisition invariants") {
    CHECK(LoadAppConfigText("acquisition: { pixel_format: Mono10Packed }").acquisition.pixel_format == "Mono10Packed");
    CHECK(Contains(ErrorOf("acquisition: { pixel_format: Mono16 }"),
                   "acquisition.pixel_format: invalid value 'Mono16'; allowed: Mono12Packed | Mono12 | Mono10Packed | Mono10 | Mono8"));
    CHECK(Contains(ErrorOf("acquisition: { spectral_binning: 3 }"), "acquisition.spectral_binning: must be 1, 2, 4, or 8"));
    CHECK(Contains(ErrorOf("acquisition: { spatial_binning: 3 }"), "acquisition.spatial_binning"));
    CHECK(LoadAppConfigText("acquisition: { spatial_binning: 8 }").acquisition.spatial_binning == 8);
    CHECK(Contains(ErrorOf("acquisition: { exposure_ms: 0 }"), "acquisition.exposure_ms: must be > 0"));
    CHECK(Contains(ErrorOf("acquisition: { frame_rate_hz: 0 }"), "acquisition.frame_rate_hz: must be > 0"));
    CHECK(Contains(ErrorOf("acquisition: { trigger: { mode: sometimes } }"),
                   "acquisition.trigger.mode: invalid value 'sometimes'; allowed: freerun | external"));
    CHECK(Contains(ErrorOf("acquisition: { trigger: { delay_ms: -1 } }"), "acquisition.trigger.delay_ms: must be in"));
    CHECK(Contains(ErrorOf("acquisition: { trigger: { source_entry: \"\" } }"), "source_entry: must not be blank"));
    const AppConfig t = LoadAppConfigText("acquisition: { trigger: { selector_entry: LineStart, source_entry: Line1 } }");
    CHECK(t.acquisition.trigger.selector_entry == "LineStart");
    CHECK(t.acquisition.trigger.source_entry == "Line1");
    // Owned by the Specim .pvcfg (EnStatusLine, AberCorrection_Enable): no longer YAML keys
    CHECK_FALSE(ErrorOf("acquisition: { status_line: false }").empty());
    CHECK_FALSE(ErrorOf("acquisition: { image_enhancement: true }").empty());
    CHECK_FALSE(ErrorOf("acquisition: { mroi: { enabled: false } }").empty());
}

TEST_CASE("config: network values that reach unsigned SDK parameters") {
    CHECK(Contains(ErrorOf("network: { retrieve_timeout_ms: -1 }"), "network.retrieve_timeout_ms"));
    CHECK(Contains(ErrorOf("network: { retrieve_timeout_ms: 0 }"), "network.retrieve_timeout_ms: must be in [1, 10000]"));
    CHECK(Contains(ErrorOf("network: { socket_rx_buffer_mb: -8 }"), "network.socket_rx_buffer_mb"));
    CHECK(Contains(ErrorOf("network: { socket_rx_buffer_mb: 2000 }"), "network.socket_rx_buffer_mb: must be in [1, 1024]"));
    CHECK(Contains(ErrorOf("network: { max_buffer_memory_mb: -1 }"), "network.max_buffer_memory_mb"));
    CHECK(Contains(ErrorOf("network: { stall_budget_s: -0.5 }"), "network.stall_budget_s"));
    CHECK(Contains(ErrorOf("network: { packet_size: -1 }"), "network.packet_size"));
    // The reconnect block was retired with the fail-fast policy: a link loss ends the rig
    CHECK(Contains(ErrorOf("network: { reconnect: { enabled: true } }"), "network.reconnect"));
    const AppConfig c = LoadAppConfigText(
        "network: { retrieve_timeout_ms: 500, socket_rx_buffer_mb: 64, max_buffer_memory_mb: 256, "
        "stall_budget_s: 0, packet_size: 0 }");
    CHECK(c.network.retrieve_timeout_ms == 500);
    CHECK(c.network.stall_budget_s == doctest::Approx(0.0));
}

TEST_CASE("config: buffer_count accepts the auto literal") {
    CHECK(LoadAppConfigText("network: { buffer_count: auto }").network.buffer_count == 0);
    CHECK(LoadAppConfigText("network: { buffer_count: 64 }").network.buffer_count == 64);
    CHECK(Contains(ErrorOf("network: { buffer_count: fast }"), "network.buffer_count: has an invalid value"));
    CHECK(Contains(ErrorOf("network: { buffer_count: -1 }"), "network.buffer_count: must be in [0, 100000]"));
}

TEST_CASE("config: output block") {
    CHECK(LoadAppConfigText("output: { flush_interval_mb: 0 }").recording.flush_interval_mb == 0u);
    CHECK(Contains(ErrorOf("output: { flush_interval_mb: -1 }"), "output.flush_interval_mb: must not be negative"));
    CHECK(Contains(ErrorOf("output: { flush_interval_mb: 5000 }"), "output.flush_interval_mb: must be in [0, 4096]"));
    CHECK(Contains(ErrorOf("output: { max_duration_s: -1 }"), "output.max_duration_s"));
    CHECK(Contains(ErrorOf("output: { max_frames: -1 }"), "output.max_frames: must not be negative"));
    CHECK(Contains(ErrorOf("output: { on_gap: skip }"), "output.on_gap: invalid value 'skip'; allowed: record | pad_zero"));
    CHECK(Contains(ErrorOf("output: { wavelengths: { grid: { start_nm: 900, end_nm: 500 } } }"),
                   "output.wavelengths.grid: start_nm must be < end_nm"));
    CHECK(Contains(ErrorOf("output: { wavelengths: { list: [400, x] } }"), "output.wavelengths.list[1]"));
    CHECK(Contains(ErrorOf("output: { wavelengths: { source: table } }"), "allowed: none | file | list | grid"));
}

TEST_CASE("config: sensor trigger") {
    // The port is the rig's (config-main.yaml); naming it here is an unknown key
    CHECK(Contains(ErrorOf("sensor_trigger: { enabled: true, port: /dev/ttyACM0 }"), "unknown key 'sensor_trigger.port'"));
    CHECK(LoadAppConfigText("sensor_trigger: { enabled: true }").sensor_trigger.enabled);
    CHECK_FALSE(LoadAppConfigText("sensor_trigger: { enabled: false }").sensor_trigger.enabled);
    CHECK(Contains(ErrorOf("sensor_trigger: { enabled: true, trigger_channel: -1 }"),
                   "sensor_trigger.trigger_channel"));
    // Pair rate limits (the firmware's PWM rule, sensor_sync_rates.h) apply only when the
    // board drives external pulses.
    CHECK(Contains(ErrorOf("acquisition: { frame_rate_hz: 17.5 }\nsensor_trigger: { enabled: true }"),
                   "acquisition.frame_rate_hz: must be within [17.882, 9847.689] Hz for the SensorSync FX group"));
    CHECK(Contains(ErrorOf("sensor_trigger: { enabled: true, trigger_channel: 4 }"),
                   "sensor_trigger.trigger_channel"));
    CHECK(Contains(ErrorOf("acquisition: { frame_rate_hz: 0.1 }\nsensor_trigger: { enabled: true, trigger_channel: 2 }"),
                   "must be within [0.140, 9847.689] Hz for the SensorSync JAI group"));
    CHECK(LoadAppConfigText("acquisition: { frame_rate_hz: 18 }\nsensor_trigger: { enabled: true, trigger_channel: 1 }")
          .acquisition.frame_rate_hz == doctest::Approx(18));
    CHECK(LoadAppConfigText("acquisition: { frame_rate_hz: 50 }\nsensor_trigger: { enabled: true, trigger_channel: 3 }")
          .acquisition.frame_rate_hz == doctest::Approx(50));
    const AppConfig freerun = LoadAppConfigText(
        "acquisition: { frame_rate_hz: 0.5, trigger: { mode: freerun } }\n"
        "sensor_trigger: { enabled: true }");
    CHECK(freerun.acquisition.frame_rate_hz == doctest::Approx(0.5));
}

TEST_CASE("config: features.raw is validated and converted at load") {
    CHECK(Contains(ErrorOf("features: { raw: [ { name: Foo, type: integer, value: 3 } ] }"),
                   "features.raw[0].type: invalid value 'integer'; allowed: int | float | bool | enum | string | command"));
    CHECK(Contains(ErrorOf("features: { raw: [ { name: Foo, type: int, value: banana } ] }"),
                   "features.raw[0].value: has an invalid value ('banana')"));
    CHECK(Contains(ErrorOf("features: { raw: [ { name: Foo, type: enum } ] }"), "features.raw[0].value: is required"));
    CHECK(Contains(ErrorOf("features: { raw: [ { type: int, value: 1 } ] }"), "features.raw[0].name: is required"));
    CHECK(Contains(ErrorOf("features: { raw: [ { name: Foo, value: 1 } ] }"), "features.raw[0].type: is required"));
    CHECK(Contains(ErrorOf("features: { raw: 3 }"), "features.raw: must be a list"));

    const AppConfig c = LoadAppConfigText(
        "features: { raw: [ { name: A, type: int, value: -7 }, { name: B, type: float, value: 1.5 }, "
        "{ name: C, type: bool, value: true }, { name: D, type: command, value: ~ } ] }");
    REQUIRE(c.features.raw.size() == 4u);
    CHECK(c.features.raw[0].int_value == -7);
    CHECK(c.features.raw[1].float_value == doctest::Approx(1.5));
    CHECK(c.features.raw[2].bool_value);
    CHECK(c.features.raw[3].value.empty());
}

TEST_CASE("config: device selection") {
    CHECK(LoadAppConfigText("device: { mac: \"00-0c-df-12-34-56\" }").device.mac == "00-0c-df-12-34-56");
    CHECK(LoadAppConfigText("device: { mac: \"000cdf123456\" }").device.mac == "000cdf123456");
    CHECK(Contains(ErrorOf("device: { mac: \"00:0c:df:12:34\" }"), "device.mac: is not a valid MAC address"));
    CHECK_THROWS_AS(LoadAppConfig("/nonexistent/path.yaml"), ConfigError);
    CHECK(LoadAppConfigText("").device.ebus_config.empty()); // default: nothing restored
    CHECK(LoadAppConfigText("device: { ebus_config: \"cfg/077000060381/a.pvcfg\" }").device.ebus_config ==
        "cfg/077000060381/a.pvcfg");
    CHECK(Contains(ErrorOf("device: { ebus_config: \"a.pvcfg\" }"), "device.ebus_config"));
    CHECK(Contains(ErrorOf("device: { ebus_config: \"cfg/077000060381/a.xml\" }"), "device.ebus_config"));
}

TEST_CASE("config: expected band count") {
    AcquisitionConfig acq;
    acq.spectral_binning = 1;
    CHECK(acq.ExpectedBands() == 448);
    acq.spectral_binning = 2;
    CHECK(acq.ExpectedBands() == 224);
    acq.spectral_binning = 8;
    CHECK(acq.ExpectedBands() == 56);
}

TEST_CASE("config: frame-rate ceiling depends on binning; exposure <= 419 ms and <= 90 % of the period") {
    CHECK(MaxFrameRateHz(1, 1) == doctest::Approx(163.02));
    for (const auto &[spatial, spectral]: {std::pair{1, 2}, {2, 1}, {2, 2}, {8, 8}}) {
        CHECK(MaxFrameRateHz(spatial, spectral) == doctest::Approx(324.373));
    }
    CHECK(kMaxExposureMs == doctest::Approx(419.0));

    // At the ceiling passes, just above it fails, in every trigger mode
    CHECK_NOTHROW(LoadAppConfigText("acquisition: { spatial_binning: 1, spectral_binning: 1, exposure_ms: 5, "
                                    "frame_rate_hz: 163.02, trigger: { mode: freerun } }"));
    CHECK(Contains(ErrorOf("acquisition: { spatial_binning: 1, spectral_binning: 1, exposure_ms: 1, "
                           "frame_rate_hz: 163.03, trigger: { mode: freerun } }"),
                   "acquisition.frame_rate_hz: must be <= 163.020 Hz"));
    CHECK_NOTHROW(LoadAppConfigText("acquisition: { spatial_binning: 1, spectral_binning: 2, exposure_ms: 1, "
                                    "frame_rate_hz: 324.373, trigger: { mode: freerun } }"));
    CHECK(Contains(ErrorOf("acquisition: { spatial_binning: 1, spectral_binning: 2, exposure_ms: 1, "
                           "frame_rate_hz: 324.374, trigger: { mode: freerun } }"),
                   "acquisition.frame_rate_hz: must be <= 324.373 Hz"));

    CHECK_NOTHROW(LoadAppConfigText("acquisition: { exposure_ms: 419, frame_rate_hz: 2, trigger: { mode: freerun } }"));
    CHECK(Contains(ErrorOf("acquisition: { exposure_ms: 419.001, frame_rate_hz: 1, trigger: { mode: freerun } }"),
                   "acquisition.exposure_ms: must be <= 419.000 ms"));

    // Exactly 90 % of the period is allowed (inclusive), anything above is not
    CHECK_NOTHROW(LoadAppConfigText("acquisition: { exposure_ms: 18, frame_rate_hz: 50, trigger: { mode: freerun } }"));
    CHECK(Contains(ErrorOf("acquisition: { exposure_ms: 18.001, frame_rate_hz: 50, trigger: { mode: freerun } }"),
                   "90 % of the frame period"));
    CHECK(ExposureFitsPeriod(18.0, 50.0));
    CHECK_FALSE(ExposureFitsPeriod(18.001, 50.0));

    // Tools re-run the same check after CLI overrides
    AcquisitionConfig acq;
    acq.spatial_binning = 2;
    acq.spectral_binning = 2;
    acq.exposure_ms = 1.0;
    acq.frame_rate_hz = 300.0;
    CHECK_NOTHROW(CheckAcquisitionTiming(acq));
    acq.spatial_binning = 1;
    acq.spectral_binning = 1;
    CHECK_THROWS_AS(CheckAcquisitionTiming(acq), ConfigError);
}
