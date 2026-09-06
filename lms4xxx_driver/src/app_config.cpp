#include "app_config.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <thread>

#include "ebus/ipv4.h"


namespace lms4xxx {
    namespace {
        using namespace common::ConfigUtil;

        bool IsIpv4(const std::string &text) {
            std::uint32_t value = 0;
            return common::Ebus::ParseIpv4(text, value);
        }

        void ParseScan(const YAML::Node &root, ScanConfig &scan) {
            const YAML::Node n = OptionalMap(root, "", "scan", {"remission"});
            ReadEnum<Remission>(n, "scan", "remission", {{"rssi", Remission::kRssi}, {"refl", Remission::kRefl}},
                                scan.remission);
        }

        void ParseNtp(const YAML::Node &root, NtpConfig &ntp) {
            const YAML::Node n = OptionalMap(root, "", "ntp", {"enabled", "server", "sync_interval_s", "check_status_s"});
            Read(n, "ntp", "enabled", ntp.enabled);
            Read(n, "ntp", "server", ntp.server);
            ReadRange<std::uint32_t>(n, "ntp", "sync_interval_s", 1, 3600, ntp.sync_interval_s);
            ReadRange<std::uint32_t>(n, "ntp", "check_status_s", 1, 3600, ntp.check_status_s);
            if (ntp.enabled && !IsIpv4(ntp.server)) {
                Fail("ntp.server", "must be a dotted-quad IPv4 address when ntp.enabled is true");
            }
        }

        void ParseNetwork(const YAML::Node &root, NetworkConfig &net) {
            const YAML::Node n = OptionalMap(root, "", "network",
                                             {"recv_buffer_bytes", "ring_buffer_frames", "receive_thread_priority",
                                              "receive_thread_cpu", "connect_timeout_ms", "response_timeout_ms",
                                              "config_timeout_ms", "tcp_keepalive", "keepalive_idle_s",
                                              "keepalive_interval_s", "keepalive_count"});
            const auto cpu_count = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
            ReadRange<std::size_t>(n, "network", "recv_buffer_bytes", 64 * 1024, 256ULL * 1024 * 1024,
                                   net.recv_buffer_bytes);
            ReadRange<std::size_t>(n, "network", "ring_buffer_frames", 16, 1024 * 1024, net.ring_buffer_frames);
            ReadRange<int>(n, "network", "receive_thread_priority", 1, 99, net.receive_thread_priority);
            ReadRange<int>(n, "network", "receive_thread_cpu", -1, cpu_count - 1, net.receive_thread_cpu);
            ReadRange<int>(n, "network", "connect_timeout_ms", 100, 60000, net.connect_timeout_ms);
            ReadRange<int>(n, "network", "response_timeout_ms", 100, 60000, net.response_timeout_ms);
            ReadRange<int>(n, "network", "config_timeout_ms", 100, 60000, net.config_timeout_ms);
            Read(n, "network", "tcp_keepalive", net.tcp_keepalive);
            ReadRange<int>(n, "network", "keepalive_idle_s", 1, 7200, net.keepalive_idle_s);
            ReadRange<int>(n, "network", "keepalive_interval_s", 1, 7200, net.keepalive_interval_s);
            ReadRange<int>(n, "network", "keepalive_count", 1, 100, net.keepalive_count);
        }

        void ParseOutput(const YAML::Node &root, OutputConfig &out) {
            const YAML::Node n = OptionalMap(root, "", "output",
                                             {"queue_max_frames", "max_file_bytes", "chunk_frames",
                                              "flush_interval_ms", "compression_level", "swmr"});
            ReadRange<std::size_t>(n, "output", "queue_max_frames", 2, 1024 * 1024, out.queue_max_frames);
            Read(n, "output", "max_file_bytes", out.max_file_bytes);
            if (out.max_file_bytes != 0 && out.max_file_bytes < 1024 * 1024) {
                Fail("output.max_file_bytes", "must be 0 (no splitting) or >= 1048576");
            }
            ReadRange<std::uint32_t>(n, "output", "chunk_frames", 1, 4096, out.chunk_frames);
            ReadRange<std::uint32_t>(n, "output", "flush_interval_ms", 1, 60000, out.flush_interval_ms);
            ReadRange<int>(n, "output", "compression_level", 0, 9, out.compression_level);
            Read(n, "output", "swmr", out.swmr);
        }

        void ParseLidars(const YAML::Node &root, std::vector<LidarConfig> &lidars) {
            const YAML::Node list = RequireSequence(root, "", "lidar");
            if (list.size() == 0) {
                Fail("lidar", "must be a non-empty list");
            }
            std::set<std::string> ids;
            std::set<std::string> addresses;
            std::size_t index = 0;
            for (const auto &item: list) {
                const std::string path = IndexPath("lidar", index++);
                CheckKeys(item, path, {"id", "enabled", "device"});

                LidarConfig cfg;
                cfg.id = RequireText(item, path, "id");
                if (cfg.id.size() > 63) {
                    Fail(path + ".id", "must be 1..63 characters");
                }
                for (const char ch: cfg.id) {
                    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
                        Fail(path + ".id", "only [A-Za-z0-9_-] allowed (used in file names)");
                    }
                }
                if (!ids.insert(cfg.id).second) {
                    Fail(path + ".id", "duplicate id \"" + cfg.id + "\"");
                }
                Read(item, path, "enabled", cfg.enabled);

                const YAML::Node device = RequireMap(item, path, "device", {"ip", "port"});
                cfg.device.ip = RequireText(device, path + ".device", "ip");
                if (!IsIpv4(cfg.device.ip)) {
                    Fail(path + ".device.ip", "\"" + cfg.device.ip + "\" is not a dotted-quad IPv4 address");
                }
                ReadRange<std::uint16_t>(device, path + ".device", "port", 1, 65535, cfg.device.port);

                // One device = one control connection: two enabled entries would fight over it
                if (cfg.enabled && !addresses.insert(cfg.device.ip).second) {
                    Fail(path + ".device.ip", "duplicate device.ip \"" + cfg.device.ip + "\" (two enabled entries)");
                }
                lidars.push_back(std::move(cfg));
            }
        }

        AppConfig ParseRoot(const YAML::Node &root) {
            CheckKeys(root, "", {"lidar", "scan", "ntp", "network", "output", "telemetry", "logging"});
            AppConfig cfg;
            ParseLidars(root, cfg.lidars);
            ParseScan(root, cfg.scan);
            ParseNtp(root, cfg.ntp);
            ParseNetwork(root, cfg.network);
            ParseOutput(root, cfg.output);
            const YAML::Node telemetry = OptionalMap(root, "", "telemetry", {"interval_s"});
            ReadRange<double>(telemetry, "telemetry", "interval_s", 0.0, 3600.0, cfg.telemetry_interval_s);
            const YAML::Node logging = OptionalMap(root, "", "logging", {"stats_interval_s"});
            ReadRange<double>(logging, "logging", "stats_interval_s", 0.0, 3600.0, cfg.stats_interval_s);
            if (cfg.EnabledLidars().empty()) {
                Fail("lidar", "at least one entry must be enabled");
            }
            return cfg;
        }
    } // namespace


    std::vector<const LidarConfig *> AppConfig::EnabledLidars() const {
        std::vector<const LidarConfig *> out;
        for (const auto &l: lidars) {
            if (l.enabled) {
                out.push_back(&l);
            }
        }
        return out;
    }


    DriverConfig AppConfig::DriverConfigFor(const LidarConfig &lidar) const {
        DriverConfig d;
        d.name = lidar.id;
        d.device = lidar.device;
        d.scan = scan;
        d.ntp = ntp;
        d.network = network;
        return d;
    }


    AppConfig LoadAppConfig(std::string_view path) {
        return ParseRoot(LoadFile(path));
    }


    AppConfig LoadAppConfigText(const std::string &yaml_text) {
        return ParseRoot(LoadText(yaml_text));
    }
} // namespace lms4xxx
