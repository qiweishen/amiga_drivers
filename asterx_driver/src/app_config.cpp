#include "app_config.h"

#include <string>
#include <vector>


namespace asterx {
    namespace {
        using namespace common::ConfigUtil;

        Vec3 ReadVec3(const YAML::Node &parent, const std::string &path, const char *key, Vec3 v) {
            const YAML::Node n = OptionalMap(parent, path, key, {"x", "y", "z"});
            const std::string p = JoinPath(path, key);
            Read(n, p, "x", v.x);
            Read(n, p, "y", v.y);
            Read(n, p, "z", v.z);
            return v;
        }

        void ParseDevice(const YAML::Node &root, AppConfig &c) {
            const YAML::Node n = RequireMap(root, "", "device", {"ip", "port", "user", "password"});
            c.host = RequireText(n, "device", "ip");
            ReadRange<std::uint16_t>(n, "device", "port", 1, 65535, c.ctrl_port);
            c.receiver.user = RequireText(n, "device", "user");
            c.receiver.password = RequireText(n, "device", "password");
        }

        void ParseOutput(const YAML::Node &root, AppConfig &c) {
            const YAML::Node n = OptionalMap(root, "", "output",
                                             {"file_prefix", "live_csv", "rotation", "write_queue_mb"});
            ReadText(n, "output", "file_prefix", c.file_prefix);
            Read(n, "output", "live_csv", c.live_csv);
            int write_queue_mb = static_cast<int>(c.write_queue_bytes >> 20);
            ReadRange<int>(n, "output", "write_queue_mb", 16, 4096, write_queue_mb);
            c.write_queue_bytes = static_cast<std::uint64_t>(write_queue_mb) << 20;
            const YAML::Node rot = OptionalMap(n, "output", "rotation", {"max_bytes", "max_interval_s"});
            // sbf2rin refuses SBF inputs of 2 GB or larger, so rotation is mandatory and capped
            ReadRange<std::uint64_t>(rot, "output.rotation", "max_bytes", 1, (2ull << 30) - 1, c.rotate_bytes);
            ReadRange<int>(rot, "output.rotation", "max_interval_s", 1, 86400 * 7, c.rotate_interval_seconds);
        }

        void ParseWarmup(const YAML::Node &r, WarmupSettings &w) {
            const YAML::Node n = OptionalMap(r, "receiver", "warmup", {"min_uptime_s", "require_finetime"});
            Read(n, "receiver.warmup", "min_uptime_s", w.min_uptime_s);
            Read(n, "receiver.warmup", "require_finetime", w.require_finetime);
        }

        void ParseNtrip(const YAML::Node &r, NtripSettings &ntrip) {
            const YAML::Node n = OptionalMap(r, "receiver", "ntrip",
                                             {"enabled", "caster", "port", "username", "password", "mount_point",
                                              "version", "tls", "send_gga_to_caster"});
            const std::string p = "receiver.ntrip";
            Read(n, p, "enabled", ntrip.enabled);
            Read(n, p, "caster", ntrip.caster);
            ReadRange<int>(n, p, "port", 1, 65535, ntrip.port);
            Read(n, p, "username", ntrip.username);
            Read(n, p, "password", ntrip.password);
            Read(n, p, "mount_point", ntrip.mount_point);
            ReadEnum(n, p, "version", {"v1", "v2"}, ntrip.version);
            Read(n, p, "tls", ntrip.tls);
            ReadEnum(n, p, "send_gga_to_caster", {"auto", "off", "sec1", "sec5", "sec10", "sec60"}, ntrip.send_gga);
        }

        void ParseAntenna(const YAML::Node &r, AntennaSettings &a) {
            const YAML::Node n = OptionalMap(r, "receiver", "antenna",
                                             {"main_antenna_type", "aux_antenna_type", "attitude_offset_deg",
                                              "antenna_lever_arm_m"});
            const std::string p = "receiver.antenna";
            ReadText(n, p, "main_antenna_type", a.main_type);
            ReadText(n, p, "aux_antenna_type", a.aux_type);
            const YAML::Node offset = OptionalMap(n, p, "attitude_offset_deg", {"heading", "pitch"});
            Read(offset, p + ".attitude_offset_deg", "heading", a.attitude_offset_deg.heading_deg);
            Read(offset, p + ".attitude_offset_deg", "pitch", a.attitude_offset_deg.pitch_deg);
            a.lever_arm_m = ReadVec3(n, p, "antenna_lever_arm_m", a.lever_arm_m);
        }

        void ParseGnss(const YAML::Node &r, GnssSettings &g) {
            const YAML::Node n = OptionalMap(r, "receiver", "gnss",
                                             {"cn0_mask_dbhz", "elevation_mask_deg", "satellite_tracking",
                                              "satellite_usage", "satellite_health_override", "signal_tracking",
                                              "signal_usage"});
            const std::string p = "receiver.gnss";
            Read(n, p, "cn0_mask_dbhz", g.cn0_mask_dbhz);
            Read(n, p, "elevation_mask_deg", g.elevation_mask_deg);
            ReadText(n, p, "satellite_tracking", g.satellite_tracking);
            ReadText(n, p, "satellite_usage", g.satellite_usage);
            ReadText(n, p, "satellite_health_override", g.satellite_health_override);
            ReadText(n, p, "signal_tracking", g.signal_tracking);
            ReadText(n, p, "signal_usage", g.signal_usage);
        }

        void ParseImu(const YAML::Node &r, ImuSettings &imu) {
            const YAML::Node n = OptionalMap(r, "receiver", "imu", {"startup_data_mode", "imu_orientation"});
            ReadEnum(n, "receiver.imu", "startup_data_mode", {"GnssTimeKnown", "Boot"}, imu.startup_data_mode);
            const YAML::Node o = OptionalMap(n, "receiver.imu", "imu_orientation",
                                             {"orientation_mode", "theta_x_deg", "theta_y_deg", "theta_z_deg"});
            const std::string p = "receiver.imu.imu_orientation";
            ReadEnum(o, p, "orientation_mode", {"SensorDefault", "manual", "fixed"}, imu.orientation_mode);
            Read(o, p, "theta_x_deg", imu.theta_x_deg);
            Read(o, p, "theta_y_deg", imu.theta_y_deg);
            Read(o, p, "theta_z_deg", imu.theta_z_deg);
        }

        void ParseNavigation(const YAML::Node &r, NavigationSettings &nav) {
            const YAML::Node n = OptionalMap(r, "receiver", "navigation",
                                             {"pvt_mode", "receiver_dynamics", "vehicle_application",
                                              "clock_sync_threshold", "multipath_mitigation", "ins_output_location"});
            const std::string p = "receiver.navigation";
            ReadText(n, p, "pvt_mode", nav.pvt_mode);
            ReadText(n, p, "receiver_dynamics", nav.receiver_dynamics);
            ReadText(n, p, "vehicle_application", nav.vehicle_application);
            ReadText(n, p, "clock_sync_threshold", nav.clock_sync_threshold);
            Read(n, p, "multipath_mitigation", nav.multipath_mitigation);
            ReadEnum(n, p, "ins_output_location", {"MainAnt", "POI1"}, nav.ins_output_location);
        }

        void ParseTimeSystem(const YAML::Node &r, TimeSystemSettings &ts) {
            const YAML::Node n = OptionalMap(r, "receiver", "time_system", {"ptp_server", "ntp_server", "pps"});
            Read(n, "receiver.time_system", "ptp_server", ts.ptp_server);
            Read(n, "receiver.time_system", "ntp_server", ts.ntp_server);
            const YAML::Node p = OptionalMap(n, "receiver.time_system", "pps",
                                             {"interval", "polarity", "delay_ns", "time_scale", "max_holdover_s",
                                              "pulse_width_ms"});
            const std::string path = "receiver.time_system.pps";
            ReadText(p, path, "interval", ts.pps.interval);
            ReadText(p, path, "polarity", ts.pps.polarity);
            Read(p, path, "delay_ns", ts.pps.delay_ns);
            ReadText(p, path, "time_scale", ts.pps.time_scale);
            Read(p, path, "max_holdover_s", ts.pps.max_holdover_s);
            Read(p, path, "pulse_width_ms", ts.pps.pulse_width_ms);
        }

        void ParseSbfStreams(const YAML::Node &r, std::vector<SbfStream> &streams) {
            const YAML::Node n = RequireSequence(r, "receiver", "sbf_streams");
            streams.clear();
            std::size_t position = 0;
            for (const auto &s: n) {
                const std::string path = IndexPath("receiver.sbf_streams", position++);
                CheckKeys(s, path, {"id", "blocks", "interval"});
                SbfStream st;
                st.stream_id = Require<int>(s, path, "id");
                st.interval = "OnChange";
                ReadText(s, path, "interval", st.interval);
                if (!Child(s, "blocks").IsDefined()) {
                    Fail(path + ".blocks", "is required");
                }
                ReadSequence(s, path, "blocks", st.blocks);
                if (st.blocks.empty()) {
                    Fail(path + ".blocks", "must name at least one SBF block or group");
                }
                streams.push_back(std::move(st));
            }
        }

        void ParseNmeaStreams(const YAML::Node &r, std::vector<NmeaPinStream> &streams) {
            const YAML::Node n = OptionalSequence(r, "receiver", "nmea_streams");
            streams.clear();
            if (!n.IsDefined()) {
                return;
            }
            int position = 0;
            for (const auto &s: n) {
                // Stream ids follow the list order: the receiver is reset on every connection
                const std::string path = IndexPath("receiver.nmea_streams", static_cast<std::size_t>(position));
                CheckKeys(s, path, {"descriptor", "messages", "interval"});
                NmeaPinStream st;
                st.stream_id = ++position;
                st.descriptor = RequireText(s, path, "descriptor");
                st.interval = "sec1";
                ReadText(s, path, "interval", st.interval);
                if (!Child(s, "messages").IsDefined()) {
                    Fail(path + ".messages", "is required");
                }
                ReadSequence(s, path, "messages", st.messages);
                if (st.messages.empty()) {
                    Fail(path + ".messages", "must name at least one NMEA sentence");
                }
                if (st.stream_id > 10) {
                    Fail("receiver.nmea_streams", "must not contain more than 10 streams");
                }
                streams.push_back(std::move(st));
            }
        }

        void ParseReceiver(const YAML::Node &root, ReceiverSettings &receiver) {
            const YAML::Node n = RequireMap(root, "", "receiver",
                                            {"warmup", "ntrip", "antenna", "gnss", "imu", "navigation", "time_system",
                                             "sbf_streams", "nmea_streams"});
            ParseWarmup(n, receiver.warmup);
            ParseNtrip(n, receiver.ntrip);
            ParseAntenna(n, receiver.antenna);
            ParseGnss(n, receiver.gnss);
            ParseImu(n, receiver.imu);
            ParseNavigation(n, receiver.navigation);
            ParseTimeSystem(n, receiver.time_system);
            ParseSbfStreams(n, receiver.sbf_streams);
            ParseNmeaStreams(n, receiver.nmea_streams);
        }

        AppConfig ParseRoot(const YAML::Node &root) {
            CheckKeys(root, "", {"device", "receiver", "output", "logging"});
            AppConfig c;
            ParseDevice(root, c);
            ParseOutput(root, c);
            const YAML::Node logging = OptionalMap(root, "", "logging", {"stats_interval_s"});
            double stats_interval_s = c.stats_period_ms / 1000.0;
            // A zero-interval QTimer spins the driver thread; Qt refuses a negative one
            ReadRange<double>(logging, "logging", "stats_interval_s", 0.001, 3600.0, stats_interval_s);
            c.stats_period_ms = static_cast<int>(stats_interval_s * 1000);
            ParseReceiver(root, c.receiver);
            ValidateReceiverSettings(c.receiver); // throws ConfigError with the dotted key
            return c;
        }
    } // namespace


    AppConfig LoadAppConfig(const std::string &path) {
        return ParseRoot(LoadFile(path));
    }


    AppConfig LoadAppConfigText(const std::string &yaml_text) {
        return ParseRoot(LoadText(yaml_text));
    }
} // namespace asterx
