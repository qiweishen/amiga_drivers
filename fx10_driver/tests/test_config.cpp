#include "../include/app_config.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

using ::testing::HasSubstr;
using fx10::Config;
using fx10::ConfigError;

namespace {
    template<typename Fn>
    std::string configErrorMessage(Fn &&fn) {
        try {
            fn();
        } catch (const ConfigError &e) {
            return e.what();
        }
        ADD_FAILURE() << "expected ConfigError";
        return "";
    }
} // namespace

// --- defaults ---------------------------------------------------------------

TEST(Config, EmptyDocumentYieldsDefaults) {
    const Config c = Config::loadFromString("");
    EXPECT_EQ(c.acquisition.pixel_format, "Mono12Packed");
    EXPECT_EQ(c.acquisition.spectral_binning, 1);
    EXPECT_EQ(c.acquisition.trigger.mode, fx10::TriggerMode::kExternal);
    EXPECT_TRUE(c.device.mac.empty());
    EXPECT_FALSE(c.device.force_ip.enabled);
    EXPECT_FALSE(c.sensor_trigger.enabled);
    EXPECT_EQ(c.sensor_trigger.trigger_channel, 0);
    EXPECT_EQ(c.network.buffer_count, 0);
    EXPECT_EQ(c.recording.on_gap, fx10::GapPolicy::kRecord);
    EXPECT_EQ(c.recording.wavelengths.source, fx10::WavelengthSource::kGrid);
    EXPECT_DOUBLE_EQ(c.watchdog.no_frame_abort_s, -1.0);
    EXPECT_EQ(c.features.map.at("exposure_time"), "ExposureTime");
    EXPECT_EQ(c.features.map.at("spectral_binning"), "BinningVertical"); // dump-verified
    EXPECT_EQ(c.features.map.at("missed_trigger_count"), "Counter1_Value");
    EXPECT_EQ(c.features.map.at("extended_id_mode"), ""); // absent on FX10e
    EXPECT_DOUBLE_EQ(c.logging.stats_interval_s, 2.5);
}

TEST(Config, FullDocumentRoundTrip) {
    const Config c = Config::loadFromString(R"(
device:
  id: "cam-01"
  mac: "00:0C:DF:12:34:56"
  ip: "10.0.0.5"
  force_ip: { enabled: true, ip: "10.0.0.6", subnet_mask: "255.255.255.0", gateway: "10.0.0.1" }
network:
  packet_size: 9000
  socket_rx_buffer_mb: 64
  buffer_count: 128
  stall_budget_s: 1.5
  max_buffer_memory_mb: 256
  retrieve_timeout_ms: 500
  reconnect: { enabled: false, max_attempts: 3, backoff_ms: 100 }
acquisition:
  pixel_format: Mono8
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
  output_dir: /tmp/out
  base_name: run7
  rotation: { max_lines: 5000, max_megabytes: 1024 }
  on_gap: pad_zero
  wavelengths:
    source: list
    list: [400.5, 500.5, 600.5]
    fwhm_list: [5.5, 5.5, 5.5]
  max_duration_s: 120.5
  max_frames: 9000
disk: { min_free_gb: 1, warn_free_gb: 4 }
watchdog: { no_frame_warn_s: 2, no_frame_abort_s: 30 }
features:
  map: { spectral_binning: "BinningVertical", trigger_mode: "TriggerModeX" }
  raw:
    - { name: TestPattern, type: enum, value: "Off" }
    - { name: DeviceReset, type: command }
logging:
  stats_interval_s: 10
)");
    EXPECT_EQ(c.device.id, "cam-01");
    EXPECT_EQ(c.device.mac, "00:0C:DF:12:34:56");
    EXPECT_EQ(c.device.ip, "10.0.0.5");
    EXPECT_TRUE(c.device.force_ip.enabled);
    EXPECT_EQ(c.device.force_ip.ip, "10.0.0.6");
    EXPECT_EQ(c.device.force_ip.subnet_mask, "255.255.255.0");
    EXPECT_EQ(c.device.force_ip.gateway, "10.0.0.1");
    EXPECT_EQ(c.network.packet_size, 9000);
    EXPECT_EQ(c.network.socket_rx_buffer_mb, 64);
    EXPECT_EQ(c.network.buffer_count, 128);
    EXPECT_DOUBLE_EQ(c.network.stall_budget_s, 1.5);
    EXPECT_FALSE(c.network.reconnect.enabled);
    EXPECT_EQ(c.network.reconnect.max_attempts, 3);
    EXPECT_EQ(c.acquisition.pixel_format, "Mono8");
    EXPECT_EQ(c.acquisition.spectral_binning, 4);
    EXPECT_TRUE(c.acquisition.status_line);
    EXPECT_EQ(c.acquisition.trigger.mode, fx10::TriggerMode::kFreerun);
    EXPECT_EQ(c.acquisition.trigger.activation, fx10::TriggerActivation::kFalling);
    EXPECT_DOUBLE_EQ(c.acquisition.trigger.delay_ms, 1.5);
    EXPECT_EQ(c.acquisition.trigger.exposure_control, fx10::ExposureControl::kPulseWidth);
    EXPECT_TRUE(c.sensor_trigger.enabled);
    EXPECT_EQ(c.sensor_trigger.port, "/dev/serial/by-id/usb-Teensyduino_Test-if00");
    EXPECT_EQ(c.sensor_trigger.trigger_channel, 2);
    EXPECT_EQ(c.recording.base_name, "run7");
    EXPECT_EQ(c.recording.rotation.max_lines, 5000u);
    EXPECT_EQ(c.recording.on_gap, fx10::GapPolicy::kPadZero);
    EXPECT_DOUBLE_EQ(c.recording.max_duration_s, 120.5);
    EXPECT_EQ(c.recording.max_frames, 9000u);
    EXPECT_EQ(c.recording.wavelengths.source, fx10::WavelengthSource::kList);
    EXPECT_EQ(c.recording.wavelengths.list.size(), 3u);
    EXPECT_EQ(c.recording.wavelengths.fwhm_list.size(), 3u);
    EXPECT_DOUBLE_EQ(c.watchdog.no_frame_abort_s, 30.0);
    // YAML overrides merge onto defaults:
    EXPECT_EQ(c.features.map.at("spectral_binning"), "BinningVertical");
    EXPECT_EQ(c.features.map.at("trigger_mode"), "TriggerModeX");
    EXPECT_EQ(c.features.map.at("exposure_time"), "ExposureTime");
    ASSERT_EQ(c.features.raw.size(), 2u);
    EXPECT_EQ(c.features.raw[0].name, "TestPattern");
    EXPECT_EQ(c.features.raw[1].type, "command");
    EXPECT_DOUBLE_EQ(c.logging.stats_interval_s, 10.0);
}

TEST(Config, ShippedTemplatesParseAndStayInSync) {
    // Both templates must parse; the effective stats interval proves the
    // logging: block is spelled right (a top-level stats_interval_s would be
    // silently ignored by the lenient parser and fall back to 2.5).
    const Config prod = Config::loadFromFile(std::string(FX10_CONFIG_DIR) + "/config-fx10.yaml");
    // trigger.mode / sensor_trigger.enabled are FIELD switches — pin only the
    // structural facts: the logging: block nests correctly (a mis-nested key
    // would silently fall back to the 2.5 default) and the trigger port is a
    // filled-in template value.
    EXPECT_NE(prod.logging.stats_interval_s, 2.5);
    EXPECT_FALSE(prod.sensor_trigger.port.empty());

    const Config snap =
            Config::loadFromFile(std::string(FX10_CONFIG_DIR) + "/config-fx10-snapshot.yaml");
    EXPECT_EQ(snap.acquisition.trigger.mode, fx10::TriggerMode::kFreerun);
    EXPECT_FALSE(snap.sensor_trigger.enabled); // snapshots never touch the trigger box
    EXPECT_DOUBLE_EQ(snap.resolvedNoFrameAbortS(), 10.0); // auto in freerun
}

// --- lenient parsing --------------------------------------------------------

TEST(Config, UnknownKeysIgnored) {
    // asterx-style leniency: unknown keys keep the defaults instead of failing
    const Config c = Config::loadFromString("bogus: 1\nnetwork: { mtu: 9000 }");
    EXPECT_EQ(c.network.packet_size, 0);
    EXPECT_EQ(c.acquisition.pixel_format, "Mono12Packed");
}

TEST(Config, InvalidScalarWrappedAsConfigError) {
    EXPECT_THAT(configErrorMessage(
                    [] { Config::loadFromString("acquisition: { exposure_ms: banana }", "test.yaml"); }),
                HasSubstr("invalid config 'test.yaml'"));
}

// --- validation -------------------------------------------------------------

TEST(Config, PixelFormatChoices) {
    EXPECT_EQ(Config::loadFromString("acquisition: { pixel_format: Mono12Packed }")
              .acquisition.pixel_format,
              "Mono12Packed");
    EXPECT_EQ(Config::loadFromString("acquisition: { pixel_format: Mono10Packed }")
              .acquisition.pixel_format,
              "Mono10Packed");
    EXPECT_THAT(configErrorMessage(
                    [] { Config::loadFromString("acquisition: { pixel_format: Mono16 }"); }),
                HasSubstr("pixel_format"));
}

TEST(Config, BinningMustBePowerOfTwoChoice) {
    EXPECT_THAT(configErrorMessage(
                    [] { Config::loadFromString("acquisition: { spectral_binning: 3 }"); }),
                HasSubstr("must be 1, 2, 4, or 8"));
}

TEST(Config, ExposureMustBePositive) {
    // Upper bounds are the camera's business (verify-set warns on clamp); only a
    // non-positive exposure is rejected here
    EXPECT_THAT(configErrorMessage(
                    [] { Config::loadFromString("acquisition: { exposure_ms: 0 }"); }),
                HasSubstr("must be > 0"));
    EXPECT_DOUBLE_EQ(Config::loadFromString("acquisition: { exposure_ms: 500 }").acquisition.exposure_ms,
                     500.0);
}

TEST(Config, TriggerEntryOverrides) {
    const Config c = Config::loadFromString(
        "acquisition: { trigger: { selector_entry: LineStart, source_entry: Line1 } }");
    EXPECT_EQ(c.acquisition.trigger.selector_entry, "LineStart");
    EXPECT_EQ(c.acquisition.trigger.source_entry, "Line1");
    EXPECT_EQ(Config::loadFromString("").acquisition.trigger.source_entry, "Line0");
}

TEST(Config, InvalidTriggerModeRejected) {
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString("acquisition: { trigger: { mode: sometimes } }");
                    }),
                HasSubstr("acquisition.trigger.mode"));
}

TEST(Config, BufferCountAcceptsAutoLiteral) {
    EXPECT_EQ(Config::loadFromString("network: { buffer_count: auto }").network.buffer_count, 0);
    EXPECT_EQ(Config::loadFromString("network: { buffer_count: 64 }").network.buffer_count, 64);
    EXPECT_THAT(
        configErrorMessage([] { Config::loadFromString("network: { buffer_count: fast }"); }),
        HasSubstr("network.buffer_count"));
}

TEST(Config, ExplicitEmptyEnumValueRejected) {
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString("acquisition: { trigger: { mode: \"\" } }");
                    }),
                HasSubstr("acquisition.trigger.mode"));
}

TEST(Config, WavelengthGridMustAscend) {
    // file/list completeness is validated by resolveWavelengths at bring-up; a
    // descending grid would silently corrupt the .hdr axis, so it stays fatal here
    EXPECT_THAT(
        configErrorMessage([] {
            Config::loadFromString(
                "output: { wavelengths: { grid: { start_nm: 900, end_nm: 500 } } }");
            }),
        HasSubstr("start_nm must be < end_nm"));
}

TEST(Config, SensorTriggerRequiresPort) {
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString("sensor_trigger: { enabled: true }");
                    }),
                HasSubstr("sensor_trigger.port"));
    // Disabled needs no port; the section may be omitted entirely.
    EXPECT_FALSE(Config::loadFromString("sensor_trigger: { enabled: false }").sensor_trigger.enabled);
}

TEST(Config, SensorTriggerChannelAndRateValidated) {
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString(
                        "sensor_trigger: { enabled: true, port: /dev/ttyACM0, trigger_channel: -1 }");
                    }),
                HasSubstr("sensor_trigger.trigger_channel"));
    // The board rejects sub-1-Hz trigger rates, so the config does too — but only
    // when the sensor trigger actually drives the camera (external mode).
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString(
                        "acquisition: { frame_rate_hz: 0.5 }\n"
                        "sensor_trigger: { enabled: true, port: /dev/ttyACM0 }");
                    }),
                HasSubstr("frame_rate_hz must be >= 1"));
    const Config freerun = Config::loadFromString(
        "acquisition: { frame_rate_hz: 0.5, trigger: { mode: freerun } }\n"
        "sensor_trigger: { enabled: true, port: /dev/ttyACM0 }");
    EXPECT_DOUBLE_EQ(freerun.acquisition.frame_rate_hz, 0.5); // freerun: no board minimum
}

TEST(Config, RawFeaturesParsed) {
    const Config c =
            Config::loadFromString("features: { raw: [ { name: DeviceReset, type: command } ] }");
    EXPECT_EQ(c.features.raw.at(0).name, "DeviceReset");
    EXPECT_EQ(c.features.raw.at(0).type, "command");
    EXPECT_TRUE(c.features.raw.at(0).value.empty());
}

TEST(Config, DiskWarnFloorMustBeAboveHardFloor) {
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString("disk: { min_free_gb: 10, warn_free_gb: 5 }");
                    }),
                HasSubstr("disk.warn_free_gb"));
}

TEST(Config, MissingFileReported) {
    EXPECT_THAT(configErrorMessage([] { Config::loadFromFile("/nonexistent/path.yaml"); }),
                HasSubstr("failed to load config"));
}

// --- device selection -------------------------------------------------------

TEST(Config, MacSelectorValidated) {
    // All common MAC spellings are accepted verbatim (normalization happens at
    // discovery-match time)
    EXPECT_EQ(Config::loadFromString("device: { mac: \"00-0c-df-12-34-56\" }").device.mac,
              "00-0c-df-12-34-56");
    EXPECT_EQ(Config::loadFromString("device: { mac: \"000cdf123456\" }").device.mac,
              "000cdf123456");
    EXPECT_THAT(configErrorMessage(
                    [] { Config::loadFromString("device: { mac: \"00:0c:df:12:34\" }"); }),
                HasSubstr("not a valid MAC address"));
}

TEST(Config, ForceIpRequiresMacAndAddress) {
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString(
                        "device: { ip: \"10.0.0.5\", force_ip: { enabled: true, ip: \"10.0.0.6\", "
                        "subnet_mask: \"255.255.255.0\" } }");
                    }),
                HasSubstr("requires device.mac"));
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString(
                        "device: { mac: \"00:0c:df:12:34:56\", force_ip: { enabled: true } }");
                    }),
                HasSubstr("requires ip and subnet_mask"));
    // Disabled force_ip needs no addresses
    EXPECT_FALSE(Config::loadFromString("device: { force_ip: { enabled: false } }")
        .device.force_ip.enabled);
}

// --- MROI -------------------------------------------------------------------

TEST(Config, MroiRequiresBinningOne) {
    EXPECT_THAT(configErrorMessage([] {
                    Config::loadFromString(
                        "acquisition: { spectral_binning: 2, mroi: { enabled: true, "
                        "multiband_string: \"0 10\" } }");
                    }),
                HasSubstr("requires spectral_binning: 1"));
}

TEST(MroiRegions, ParsesValidString) {
    const auto regions = fx10::parseMroiRegions("0 10;20 30; 100 5 ;");
    ASSERT_EQ(regions.size(), 3u);
    EXPECT_EQ(regions[0], std::make_pair(0, 10));
    EXPECT_EQ(regions[1], std::make_pair(20, 30));
    EXPECT_EQ(regions[2], std::make_pair(100, 5));
}

TEST(MroiRegions, RejectsMalformedInput) {
    EXPECT_THROW(fx10::parseMroiRegions(""), ConfigError);
    EXPECT_THROW(fx10::parseMroiRegions("10"), ConfigError); // missing height
    EXPECT_THROW(fx10::parseMroiRegions("10 20 30"), ConfigError); // extra token
    EXPECT_THROW(fx10::parseMroiRegions("0 0"), ConfigError); // zero height
    EXPECT_THROW(fx10::parseMroiRegions("-1 10"), ConfigError); // negative start
    EXPECT_THROW(fx10::parseMroiRegions("1082 1"), ConfigError); // start beyond sensor
    EXPECT_THROW(fx10::parseMroiRegions("1000 200"), ConfigError); // start+height > 1082
    // each region valid, but total 1200 > 1082 native rows
    EXPECT_THROW(fx10::parseMroiRegions("0 400;300 400;600 400"), ConfigError);
    // native-coordinate regions beyond the old 448 limit are legal now
    EXPECT_EQ(fx10::parseMroiRegions("600 100").size(), 1u);
}

// --- derived values ---------------------------------------------------------

TEST(Config, ExpectedBandsFromBinning) {
    fx10::AcquisitionConfig acq;
    acq.spectral_binning = 1;
    EXPECT_EQ(acq.expectedBands(), 448);
    acq.spectral_binning = 2;
    EXPECT_EQ(acq.expectedBands(), 224);
    acq.spectral_binning = 4;
    EXPECT_EQ(acq.expectedBands(), 112);
    acq.spectral_binning = 8;
    EXPECT_EQ(acq.expectedBands(), 56);
}

TEST(Config, ExpectedBandsFromMroi) {
    fx10::AcquisitionConfig acq;
    acq.spectral_binning = 1;
    acq.mroi.enabled = true;
    acq.mroi.multiband_string = "0 10;20 30";
    EXPECT_EQ(acq.expectedBands(), 40);
}

TEST(Config, WatchdogAutoResolution) {
    Config external = Config::loadFromString("acquisition: { trigger: { mode: external } }");
    EXPECT_DOUBLE_EQ(external.resolvedNoFrameAbortS(), 0.0);
    Config freerun = Config::loadFromString("acquisition: { trigger: { mode: freerun } }");
    EXPECT_DOUBLE_EQ(freerun.resolvedNoFrameAbortS(), 10.0);
    Config explicit_abort = Config::loadFromString("watchdog: { no_frame_abort_s: 30 }");
    EXPECT_DOUBLE_EQ(explicit_abort.resolvedNoFrameAbortS(), 30.0);
}

TEST(Config, FeatureNodeAccessor) {
    const Config c = Config::loadFromString("");
    EXPECT_EQ(c.features.node("exposure_time"), "ExposureTime");
    EXPECT_EQ(c.features.node("mroi_enable"), "MROI_Enable");
    EXPECT_EQ(c.features.node("extended_id_mode"), "");
    EXPECT_THROW(c.features.node("bogus_role"), ConfigError);
}
