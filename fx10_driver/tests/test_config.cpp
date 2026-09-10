// app_config.cpp: strict schema, defaults, every invariant, the shipped templates.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

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
    CHECK(c.acquisition.image_enhancement);
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
  status_line: true
  trigger: { mode: freerun, activation: falling, delay_ms: 1.5, exposure_control: pulse_width }
sensor_trigger:
  enabled: true
  port: /dev/serial/by-id/usb-Teensyduino_Test-if00
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
    CHECK(c.acquisition.status_line);
    CHECK(c.acquisition.trigger.mode == TriggerMode::kFreerun);
    CHECK(c.acquisition.trigger.activation == TriggerActivation::kFalling);
    CHECK(c.acquisition.trigger.delay_ms == doctest::Approx(1.5));
    CHECK(c.acquisition.trigger.exposure_control == ExposureControl::kPulseWidth);
    CHECK(c.sensor_trigger.enabled);
    CHECK(c.sensor_trigger.port == "/dev/serial/by-id/usb-Teensyduino_Test-if00");
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
    CHECK_FALSE(prod.sensor_trigger.port.empty());
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
        CHECK(c->acquisition.image_enhancement);
        CHECK_FALSE(c->acquisition.status_line);
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
    // AIE off needs a node name the manual never prints
    CHECK(Contains(ErrorOf("acquisition: { image_enhancement: false }"), "acquisition.image_enhancement: only true is supported"));
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
    CHECK(Contains(ErrorOf("sensor_trigger: { enabled: true }"), "sensor_trigger.port: is required"));
    CHECK_FALSE(LoadAppConfigText("sensor_trigger: { enabled: false }").sensor_trigger.enabled);
    CHECK(Contains(ErrorOf("sensor_trigger: { enabled: true, port: /dev/ttyACM0, trigger_channel: -1 }"),
                   "sensor_trigger.trigger_channel"));
    // The board rejects sub-1-Hz rates, but only external mode drives it
    CHECK(Contains(ErrorOf("acquisition: { frame_rate_hz: 0.5 }\nsensor_trigger: { enabled: true, port: /dev/ttyACM0 }"),
                   "acquisition.frame_rate_hz: must be >= 1"));
    const AppConfig freerun = LoadAppConfigText(
        "acquisition: { frame_rate_hz: 0.5, trigger: { mode: freerun } }\n"
        "sensor_trigger: { enabled: true, port: /dev/ttyACM0 }");
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
}

TEST_CASE("config: MROI requires 1 x 1 binning on both axes") {
    CHECK(Contains(ErrorOf("acquisition: { spectral_binning: 2, mroi: { enabled: true, multiband_string: \"0 10\" } }"),
                   "acquisition.mroi: requires 1 x 1 binning"));
    CHECK(Contains(ErrorOf("acquisition: { spatial_binning: 2, spectral_binning: 1, mroi: { enabled: true, "
                           "multiband_string: \"0 10\" } }"),
                   "spatial_binning"));
    CHECK(LoadAppConfigText("acquisition: { spatial_binning: 1, spectral_binning: 1, mroi: { enabled: true, "
                            "multiband_string: \"0 10\" } }").acquisition.mroi.enabled);
}

TEST_CASE("mroi: region string parsing") {
    const auto regions = ParseMroiRegions("0 10;20 30; 100 5 ;");
    REQUIRE(regions.size() == 3u);
    CHECK(regions[0] == std::make_pair(0, 10));
    CHECK(regions[1] == std::make_pair(20, 30));
    CHECK(regions[2] == std::make_pair(100, 5));

    CHECK_THROWS_AS(ParseMroiRegions(""), ConfigError);
    CHECK_THROWS_AS(ParseMroiRegions("10"), ConfigError);
    CHECK_THROWS_AS(ParseMroiRegions("10 20 30"), ConfigError);
    CHECK_THROWS_AS(ParseMroiRegions("0 0"), ConfigError);
    CHECK_THROWS_AS(ParseMroiRegions("-1 10"), ConfigError);
    CHECK_THROWS_AS(ParseMroiRegions("1082 1"), ConfigError);
    CHECK_THROWS_AS(ParseMroiRegions("1000 200"), ConfigError);

    // Start row is a native sensor index; height and total are bounded by the 448-row window (p.26)
    CHECK(ParseMroiRegions("600 100").size() == 1u);
    CHECK(ParseMroiRegions("500 448").at(0).second == 448);
    CHECK_THROWS_WITH_AS(ParseMroiRegions("100 449"), doctest::Contains("[1, 448]"), ConfigError);
    CHECK_THROWS_WITH_AS(ParseMroiRegions("0 300;400 200"), doctest::Contains("448 rows"), ConfigError);
    CHECK_NOTHROW(ParseMroiRegions("0 300;400 148"));
}

TEST_CASE("config: expected band count") {
    AcquisitionConfig acq;
    acq.spectral_binning = 1;
    CHECK(acq.ExpectedBands() == 448);
    acq.spectral_binning = 2;
    CHECK(acq.ExpectedBands() == 224);
    acq.spectral_binning = 8;
    CHECK(acq.ExpectedBands() == 56);
    acq.spectral_binning = 1;
    acq.mroi.enabled = true;
    acq.mroi.multiband_string = "0 10;20 30";
    CHECK(acq.ExpectedBands() == 40);
}
