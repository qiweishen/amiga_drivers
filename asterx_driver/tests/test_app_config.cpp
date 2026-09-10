// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "app_config.h"

using namespace asterx;

namespace {
    // A complete, valid configuration; variants replace one fragment
    const char *kBase = R"yaml(
device:
    ip: "192.0.2.10"
    port: 28784
    user: "amiga"
    password: "secret"
receiver:
    warmup: { min_uptime_s: 1200, require_finetime: true }
    ntrip: { enabled: false, caster: www.example.com, port: 2101, username: "<?>", password: "<?>", mount_point: MP, version: v2, tls: false, send_gga_to_caster: auto }
    antenna:
        main_antenna_type: Unknown
        aux_antenna_type: Unknown
        attitude_offset_deg: { heading: 4.0, pitch: -5.0 }
        antenna_lever_arm_m: { x: 0.1, y: -0.2, z: 0.3 }
    gnss: { cn0_mask_dbhz: 12, elevation_mask_deg: 5, satellite_tracking: all, satellite_usage: all, satellite_health_override: none, signal_tracking: all, signal_usage: all }
    imu:
        startup_data_mode: GnssTimeKnown
        imu_orientation: { orientation_mode: SensorDefault, theta_x_deg: 0.0, theta_y_deg: 0.0, theta_z_deg: 0.0 }
    navigation: { pvt_mode: all, receiver_dynamics: Moderate, vehicle_application: Unknown, clock_sync_threshold: usec500, multipath_mitigation: true, ins_output_location: POI1 }
    time_system:
        ptp_server: true
        ntp_server: true
        pps: { interval: sec1, polarity: Low2High, delay_ns: 0.00, time_scale: GPS, max_holdover_s: 60, pulse_width_ms: 5.000000 }
    sbf_streams:
        - { id: 1, blocks: [ Measurements ], interval: OnChange }
        - { id: 8, blocks: [ Status ], interval: OnChange }
    nmea_streams:
        - { descriptor: COM1, messages: [ ZDA ], interval: sec1 }
output:
    file_prefix: asterx
    live_csv: true
    write_queue_mb: 256
    rotation: { max_bytes: 1073741824, max_interval_s: 3600 }
logging:
    stats_interval_s: 2.5
)yaml";

    std::string Replaced(const std::string &from, const std::string &to) {
        std::string s = kBase;
        const auto pos = s.find(from);
        REQUIRE_MESSAGE(pos != std::string::npos, "fixture fragment not found: " << from);
        return s.replace(pos, from.size(), to);
    }

    // "" when the config loads, otherwise the error message
    std::string ErrorOf(const std::string &yaml) {
        try {
            (void) LoadAppConfigText(yaml);
        } catch (const ConfigError &e) {
            return e.what();
        }
        return {};
    }

    bool Mentions(const std::string &error, const std::string &needle) {
        return error.find(needle) != std::string::npos;
    }

    bool HasCommand(const std::vector<Command> &cmds, const std::string &text) {
        return std::any_of(cmds.begin(), cmds.end(), [&text](const Command &c) { return c.text == text; });
    }
} // namespace

TEST_CASE("config: the complete schema loads") {
    const auto cfg = LoadAppConfigText(kBase);
    CHECK(cfg.host == "192.0.2.10");
    CHECK(cfg.ctrl_port == 28784);
    CHECK(cfg.receiver.user == "amiga");
    CHECK(cfg.receiver.password == "secret");
    CHECK(cfg.receiver.warmup.min_uptime_s == 1200);
    CHECK(cfg.receiver.warmup.require_finetime);
    CHECK_FALSE(cfg.receiver.ntrip.enabled);
    CHECK(cfg.receiver.ntrip.version == "v2");
    CHECK(cfg.receiver.antenna.attitude_offset_deg.pitch_deg == doctest::Approx(-5.0));
    CHECK(cfg.receiver.antenna.lever_arm_m.y == doctest::Approx(-0.2));
    CHECK(cfg.receiver.gnss.cn0_mask_dbhz == 12);
    CHECK(cfg.receiver.imu.orientation_mode == "SensorDefault");
    CHECK(cfg.receiver.navigation.ins_output_location == "POI1");
    CHECK(cfg.receiver.time_system.ptp_server);
    CHECK(cfg.receiver.time_system.pps.max_holdover_s == 60);
    REQUIRE(cfg.receiver.sbf_streams.size() == 2u);
    CHECK(cfg.receiver.sbf_streams[1].stream_id == 8);
    REQUIRE(cfg.receiver.nmea_streams.size() == 1u);
    CHECK(cfg.receiver.nmea_streams[0].stream_id == 1); // ids follow the list order
    CHECK(cfg.receiver.nmea_streams[0].descriptor == "COM1");
    CHECK(cfg.file_prefix == "asterx");
    CHECK(cfg.live_csv);
    CHECK(cfg.rotate_bytes == 1073741824ull);
    CHECK(cfg.rotate_interval_seconds == 3600);
    CHECK(cfg.stats_period_ms == 2500);
    CHECK(cfg.output_dir.empty()); // injected by the host
}

TEST_CASE("config: the shipped yaml loads and drives the real sequence") {
    const auto cfg = LoadAppConfig(std::string(ASTERX_CONFIG_DIR) + "/config-asterx.yaml");
    CHECK(cfg.receiver.time_system.pps.polarity == "Low2High");
    CHECK_FALSE(cfg.receiver.ntrip.enabled); // placeholders until credentials are filled in
    // This profile explicitly opts out of both gates; omitted-key defaults
    // remain 1200 s / FINETIME and are tested independently below.
    CHECK(cfg.receiver.warmup.min_uptime_s == 0);
    CHECK_FALSE(cfg.receiver.warmup.require_finetime);
    CHECK(cfg.stats_period_ms == 2500);
    CHECK(cfg.rotate_bytes == 1073741824ull);
    CHECK(cfg.rotate_interval_seconds == 3600);
    CHECK(cfg.live_csv);
    REQUIRE(cfg.receiver.sbf_streams.size() >= 9u);

    const auto cmds = BuildCommandList(cfg.receiver, "IP12");
    CHECK(HasCommand(cmds, "setNTPServer, on"));
    CHECK(HasCommand(cmds, "setPTPServer, on"));
    CHECK(HasCommand(cmds, "setNMEAOutput, Stream1, COM1, ZDA, sec1"));
    CHECK(HasCommand(cmds, "setDataInOut, COM1, , +NMEA"));
    CHECK(HasCommand(cmds, "setINSNavConfig, on, all, POI1"));
    CHECK(HasCommand(cmds, "setNtripSettings, NTR1, off"));
    CHECK(HasCommand(cmds, "setPPSParameters, sec1, Low2High, 0.00, GPS, 60, 5.000000"));
    CHECK(static_cast<std::size_t>(std::count_if(cmds.begin(), cmds.end(), [](const Command &c) {
        return c.text.rfind("setSBFOutput, Stream", 0) == 0;
    })) == cfg.receiver.sbf_streams.size());
}

TEST_CASE("config: unknown keys are errors naming the key and the accepted set") {
    // ptp_server defaults to false, so a typo would silently send setPTPServer, off
    const auto err = ErrorOf(Replaced("ptp_server: true", "ptp_sever: true"));
    CHECK(Mentions(err, "unknown key 'receiver.time_system.ptp_sever'"));
    CHECK(Mentions(err, "accepts only: ptp_server, ntp_server, pps"));

    CHECK(Mentions(ErrorOf(Replaced("min_uptime_s: 1200", "min_uptime_sec: 1200")), "receiver.warmup.min_uptime_sec"));
    CHECK(Mentions(ErrorOf(Replaced("    navigation:", "    navigatoin:")), "unknown key 'receiver.navigatoin'"));
    CHECK(Mentions(ErrorOf(std::string(kBase) + "unexpected_top_level: 1\n"), "unknown key 'unexpected_top_level'"));
    CHECK(Mentions(ErrorOf(std::string(kBase) + "stats_period_s: 1\n"), "unknown key 'stats_period_s'"));
    CHECK(Mentions(ErrorOf(Replaced("    navigation:", "    streams:")), "unknown key 'receiver.streams'"));
    CHECK(Mentions(ErrorOf(Replaced("max_holdover_s: 60", "max_holdover_sec: 60")), "pps.max_holdover_sec"));
    CHECK(Mentions(ErrorOf(Replaced("    file_prefix: asterx", "    output_dir: /tmp\n    file_prefix: asterx")),
                   "unknown key 'output.output_dir'"));
    CHECK(Mentions(ErrorOf(Replaced("rotation: { max_bytes: 1073741824, max_interval_s: 3600 }",
                                    "rotate_bytes: 1073741824")),
                   "unknown key 'output.rotate_bytes'"));
}

TEST_CASE("config: required keys, blank and null values") {
    // A key without a value is a Null node whose as<string>() would be the literal "null"
    const auto blank = ErrorOf(Replaced("user: \"amiga\"", "user:"));
    CHECK(Mentions(blank, "device.user: has an invalid value"));
    CHECK_FALSE(Mentions(blank, "null"));

    CHECK(Mentions(ErrorOf(Replaced("password: \"secret\"", "password: \"\"")), "device.password: must not be blank"));
    CHECK(Mentions(ErrorOf(Replaced("    ip: \"192.0.2.10\"\n", "")), "device.ip: is required"));
    CHECK(Mentions(ErrorOf(Replaced("    sbf_streams:\n        - { id: 1, blocks: [ Measurements ], interval: OnChange }\n"
                                    "        - { id: 8, blocks: [ Status ], interval: OnChange }\n", "")),
                   "receiver.sbf_streams: is required"));
    CHECK(Mentions(ErrorOf("receiver:\n    sbf_streams: []\n"), "device: is required"));
    CHECK(Mentions(ErrorOf(Replaced("{ x: 0.1, y: -0.2, z: 0.3 }", "{ x: 0.1, y: -0.2, w: 1 }")),
                   "unknown key 'receiver.antenna.antenna_lever_arm_m.w'"));
    CHECK(Mentions(ErrorOf(Replaced("min_uptime_s: 1200", "min_uptime_s:")), "receiver.warmup.min_uptime_s: has an invalid value"));
}

TEST_CASE("config: omitted optional blocks keep the defaults") {
    const auto cfg = LoadAppConfigText(R"yaml(
device: { ip: "192.0.2.10", user: amiga, password: secret }
receiver:
    sbf_streams:
        - { id: 8, blocks: [ Status ] }
)yaml");
    CHECK(cfg.ctrl_port == 28784);
    CHECK(cfg.receiver.antenna.aux_type == "Unknown");
    CHECK(cfg.receiver.antenna.lever_arm_m.z == doctest::Approx(0.0));
    CHECK(cfg.receiver.warmup.min_uptime_s == 1200);
    CHECK(cfg.receiver.time_system.pps.interval == "sec1");
    CHECK(cfg.receiver.warmup.require_finetime);
    CHECK(cfg.receiver.sbf_streams[0].interval == "OnChange");
    CHECK(cfg.receiver.nmea_streams.empty());
    CHECK(cfg.rotate_bytes == (1ull << 30));
    CHECK(cfg.stats_period_ms == 2500);
}

TEST_CASE("config: invalid values are rejected with the key path") {
    CHECK(Mentions(ErrorOf(Replaced("port: 28784", "port: 70000")), "device.port: must be in [1, 65535]"));
    CHECK(Mentions(ErrorOf(Replaced("port: 28784", "port: not-a-number")), "device.port: has an invalid value"));
    CHECK(Mentions(ErrorOf(Replaced("polarity: Low2High", "polarity: Low2Hight")), "Low2High"));
    CHECK_FALSE(ErrorOf(Replaced("interval: sec1, polarity", "interval: sec3, polarity")).empty());
    CHECK(Mentions(ErrorOf(Replaced("max_holdover_s: 60", "max_holdover_s: 3601")), "pps.max_holdover_s"));
    CHECK(Mentions(ErrorOf(Replaced("pulse_width_ms: 5.000000", "pulse_width_ms: 0")), "pps.pulse_width_ms"));
    CHECK(Mentions(ErrorOf(Replaced("delay_ns: 0.00", "delay_ns: 2000000")), "pps.delay_ns"));
    CHECK_FALSE(ErrorOf(Replaced("time_scale: GPS", "time_scale: TAI")).empty());
    CHECK_FALSE(ErrorOf(Replaced("receiver_dynamics: Moderate", "receiver_dynamics: Extreme")).empty());
    CHECK_FALSE(ErrorOf(Replaced("vehicle_application: Unknown", "vehicle_application: Boat")).empty());
    CHECK_FALSE(ErrorOf(Replaced("clock_sync_threshold: usec500", "clock_sync_threshold: msec9")).empty());
    CHECK(Mentions(ErrorOf(Replaced("ins_output_location: POI1", "ins_output_location: POI2")), "allowed: MainAnt | POI1"));
    CHECK(Mentions(ErrorOf(Replaced("startup_data_mode: GnssTimeKnown", "startup_data_mode: Cold")), "allowed: GnssTimeKnown | Boot"));
    CHECK(Mentions(ErrorOf(Replaced("version: v2", "version: v3")), "receiver.ntrip.version: invalid value 'v3'"));
    CHECK_FALSE(ErrorOf(Replaced("pvt_mode: all", "pvt_mode: RTKFixed+Foo")).empty());
    CHECK(ErrorOf(Replaced("pvt_mode: all", "pvt_mode: StandAlone+RTK")).empty());
    CHECK_FALSE(ErrorOf(Replaced("elevation_mask_deg: 5", "elevation_mask_deg: 91")).empty());
    CHECK_FALSE(ErrorOf(Replaced("signal_usage: all", "signal_usage: GPS")).empty()); // alias not allowed
    CHECK(ErrorOf(Replaced("signal_tracking: all", "signal_tracking: GPS+GALE5a")).empty());
    CHECK_FALSE(ErrorOf(Replaced("satellite_tracking: all", "satellite_tracking: G37")).empty());
    CHECK_FALSE(ErrorOf(Replaced("satellite_usage: all", "satellite_usage: G33")).empty()); // usage stops at G32
    CHECK(ErrorOf(Replaced("satellite_tracking: all", "satellite_tracking: G38")).empty());
    CHECK_FALSE(ErrorOf(Replaced("min_uptime_s: 1200", "min_uptime_s: -1")).empty());
    CHECK(ErrorOf(Replaced("min_uptime_s: 1200", "min_uptime_s: 0")).empty());
    CHECK_FALSE(ErrorOf(Replaced("theta_y_deg: 0.0", "theta_y_deg: 91.0")).empty());
    CHECK_FALSE(ErrorOf(Replaced("theta_x_deg: 0.0", "theta_x_deg: 181.0")).empty());
    CHECK(Mentions(ErrorOf(Replaced("z: 0.3 }", "z: 101 }")), "antenna_lever_arm_m.z"));
    // p.87 credential limits
    CHECK_FALSE(ErrorOf(Replaced("user: \"amiga\"", "user: \"seventeen_chars_x\"")).empty());
    CHECK_FALSE(ErrorOf(Replaced("user: \"amiga\"", "user: \"RxAdmin\"")).empty());
    CHECK_FALSE(ErrorOf(Replaced("user: \"amiga\"", "user: \"rxadmin\"")).empty());
}

TEST_CASE("config: stream rules") {
    // At least one stream must carry ReceiverStatus for the warm-up gate
    CHECK(Mentions(ErrorOf(Replaced("        - { id: 8, blocks: [ Status ], interval: OnChange }\n", "")), "ReceiverStatus"));
    CHECK_FALSE(ErrorOf(Replaced("- { id: 1, blocks: [ Measurements ], interval: OnChange }",
                                 "- { id: 8, blocks: [ Measurements ], interval: OnChange }")).empty());
    CHECK_FALSE(ErrorOf(Replaced("blocks: [ Status ], interval: OnChange }", "blocks: [ Status ], interval: sec3 }")).empty());
    // "off" would leave the warm-up gate closed for ever
    CHECK_FALSE(ErrorOf(Replaced("blocks: [ Status ], interval: OnChange }", "blocks: [ Status ], interval: off }")).empty());
    CHECK(Mentions(ErrorOf(Replaced("- { id: 8, blocks: [ Status ], interval: OnChange }", "- { blocks: [ Status ], interval: OnChange }")),
                   "receiver.sbf_streams[1].id: is required"));
    CHECK(Mentions(ErrorOf(Replaced("blocks: [ Status ]", "blocks: []")), "receiver.sbf_streams[1].blocks"));
    CHECK(Mentions(ErrorOf(Replaced("- { id: 8, blocks: [ Status ], interval: OnChange }", "- { id: 8, interval: OnChange }")),
                   "receiver.sbf_streams[1].blocks: is required"));

    const auto two = LoadAppConfigText(Replaced("        - { descriptor: COM1, messages: [ ZDA ], interval: sec1 }",
                                                "        - { descriptor: COM1, messages: [ ZDA ], interval: sec1 }\n"
                                                "        - { descriptor: COM2, messages: [ GGA, RMC ], interval: sec1 }"));
    REQUIRE(two.receiver.nmea_streams.size() == 2u);
    CHECK(two.receiver.nmea_streams[0].stream_id == 1);
    CHECK(two.receiver.nmea_streams[1].stream_id == 2);
    CHECK(two.receiver.nmea_streams[1].messages.size() == 2u);

    CHECK(Mentions(ErrorOf(Replaced("{ descriptor: COM1, messages: [ ZDA ], interval: sec1 }",
                                    "{ id: 3, descriptor: COM1, messages: [ ZDA ], interval: sec1 }")),
                   "unknown key 'receiver.nmea_streams[0].id'"));
    CHECK(Mentions(ErrorOf(Replaced("messages: [ ZDA ]", "message: ZDA")), "unknown key 'receiver.nmea_streams[0].message'"));
    CHECK(Mentions(ErrorOf(Replaced("    nmea_streams:\n        - { descriptor: COM1, messages: [ ZDA ], interval: sec1 }\n",
                                    "    nmea_streams:\n        descriptor: COM1\n")),
                   "receiver.nmea_streams: must be a list"));
    CHECK_FALSE(ErrorOf(Replaced("messages: [ ZDA ]", "messages: [ GSV ]")).empty());
    CHECK_FALSE(ErrorOf(Replaced("descriptor: COM1", "descriptor: COM3")).empty());
    const auto none = LoadAppConfigText(Replaced("    nmea_streams:\n        - { descriptor: COM1, messages: [ ZDA ], interval: sec1 }\n",
                                                 "    nmea_streams: []\n"));
    CHECK(none.receiver.nmea_streams.empty());
}

TEST_CASE("config: output and logging") {
    CHECK(Mentions(ErrorOf(Replaced("stats_interval_s: 2.5", "stats_interval_s: 0")), "logging.stats_interval_s: must be in"));
    CHECK(Mentions(ErrorOf(Replaced("stats_interval_s: 2.5", "stats_interval_s: -1")), "logging.stats_interval_s"));
    CHECK(LoadAppConfigText(Replaced("stats_interval_s: 2.5", "stats_interval_s: 1")).stats_period_ms == 1000);

    CHECK(Mentions(ErrorOf(Replaced("max_bytes: 1073741824", "max_bytes: 2147483648")), "output.rotation.max_bytes: must be in"));
    CHECK(Mentions(ErrorOf(Replaced("max_bytes: 1073741824", "max_bytes: 0")), "output.rotation.max_bytes"));
    CHECK(Mentions(ErrorOf(Replaced("max_interval_s: 3600", "max_interval_s: 0")), "output.rotation.max_interval_s"));
    CHECK(Mentions(ErrorOf(Replaced("file_prefix: asterx", "file_prefix: \"\"")), "output.file_prefix: must not be blank"));
    CHECK_FALSE(LoadAppConfigText(Replaced("live_csv: true", "live_csv: false")).live_csv);

    CHECK(LoadAppConfigText(kBase).write_queue_bytes == (256ull << 20));
    CHECK(LoadAppConfigText(Replaced("write_queue_mb: 256", "write_queue_mb: 64")).write_queue_bytes == (64ull << 20));
    CHECK(Mentions(ErrorOf(Replaced("write_queue_mb: 256", "write_queue_mb: 8")), "output.write_queue_mb"));
    CHECK(Mentions(ErrorOf(Replaced("write_queue_mb: 256", "write_queue_mb: 5000")), "output.write_queue_mb"));
}

TEST_CASE("config: a missing file reports the path") {
    CHECK_THROWS_WITH_AS(LoadAppConfig("/nonexistent/asterx.yaml"), doctest::Contains("/nonexistent/asterx.yaml"), ConfigError);
}
