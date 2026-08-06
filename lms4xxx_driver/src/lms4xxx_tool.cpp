#include "lms4xxx_tool.h"

#include <cctype>
#include <set>

#include "logger.h"
#include "utility.h"


namespace {
    constexpr std::string_view kModule = "LMS4xxx";
    Common::DriverLog g_log{std::string(kModule)};
} // namespace


namespace LMS4xxxTool {
    std::vector<LiDARConfig> LoadConfigs(std::string_view config_path, std::string &error) {
        std::vector<LiDARConfig> configs;
        error.clear();

        try {
        Common::ConfigLoader loader(config_path);
        const auto &root = loader.root();

        // Shared scan config
        LMS4xxx::ScanConfig shared_scan;
        if (const auto &scan = root["scan"]) {
            shared_scan.start_angle_deg = scan["start_angle_deg"].as<double>(55.0);
            shared_scan.stop_angle_deg = scan["stop_angle_deg"].as<double>(125.0);
            shared_scan.angular_resolution_deg = scan["angular_resolution_deg"].as<double>(0.0833);
            shared_scan.enable_distance = scan["enable_distance"].as<bool>(true);
            shared_scan.enable_rssi = scan["enable_rssi"].as<bool>(false);
            shared_scan.enable_reflectance = scan["enable_reflectance"].as<bool>(false);
            shared_scan.enable_angle_correction = scan["enable_angle_correction"].as<bool>(false);
            shared_scan.enable_quality = scan["enable_quality"].as<bool>(false);
            shared_scan.output_rate = scan["output_rate"].as<std::uint16_t>(1);
        }

        // Shared network config
        LMS4xxx::NetworkConfig shared_network;
        if (const auto &network = root["network"]) {
            shared_network.recv_buffer_bytes = network["recv_buffer_bytes"].as<std::size_t>(4 * 1024 * 1024);
            shared_network.ring_buffer_frames = network["ring_buffer_frames"].as<std::size_t>(1024);
            shared_network.receive_thread_priority = network["receive_thread_priority"].as<int>(99);
            shared_network.receive_thread_cpu = network["receive_thread_cpu"].as<int>(-1);
            shared_network.connect_timeout_ms = network["connect_timeout_ms"].as<int>(5000);
            shared_network.response_timeout_ms = network["response_timeout_ms"].as<int>(2000);
            shared_network.tcp_keepalive = network["tcp_keepalive"].as<bool>(true);
            shared_network.keepalive_idle_s = network["keepalive_idle_s"].as<int>(10);
            shared_network.keepalive_interval_s = network["keepalive_interval_s"].as<int>(5);
            shared_network.keepalive_count = network["keepalive_count"].as<int>(3);
        }

        // Time sync
        bool shared_enable_ntp = false;
        std::string shared_ntp_server;
        double sync_interval_s = 1.0;
        double check_status_s = 5.0;
        if (const auto &ntp = root["ntp"]) {
            shared_enable_ntp = ntp["enabled"].as<bool>(false);
            shared_ntp_server = ntp["server"].as<std::string>("");
            sync_interval_s = ntp["sync_interval_s"].as<double>(1.0);
            check_status_s = ntp["check_status_s"].as<double>(5.0);
        }

        // Output
        std::size_t shared_queue_max_frames = 512;
        std::size_t shared_write_buffer_bytes = 256 * 1024;
        std::size_t shared_max_file_bytes = 1ULL * 1024 * 1024 * 1024;
        if (const auto &output = root["output"]) {
            shared_queue_max_frames = output["queue_max_frames"].as<std::size_t>(512);
            shared_write_buffer_bytes = output["write_buffer_bytes"].as<std::size_t>(256 * 1024);
            shared_max_file_bytes = output["max_file_bytes"].as<std::size_t>(1ULL * 1024 * 1024 * 1024);
        }

        // Logging
        double stats_interval_s = 2.5;
        if (const auto &logging = root["logging"]) {
            stats_interval_s = logging["stats_interval_s"].as<double>(2.5);
        }

        // Per-instance configuration: gox-style `lidar:` sequence — one entry
        // per LiDAR ({id, enabled, device{ip,port}}), shared sections apply to
        // all. The id is used verbatim as the log tag and in the recording
        // filename, hence the gox-identical character rules.
        const auto &lidars = root["lidar"];
        if (!lidars || !lidars.IsSequence() || lidars.size() == 0) {
            error = "lidar must be a non-empty list";
            return {};
        }

        std::set<std::string> ids;
        std::size_t idx = 0;
        for (const auto &item: lidars) {
            const std::string tag = "lidar[" + std::to_string(idx) + "]";
            ++idx;

            LiDARConfig cfg;
            if (item["id"]) {
                cfg.position_name = item["id"].as<std::string>();
            }
            if (cfg.position_name.empty() || cfg.position_name.size() > 63) {
                error = tag + ".id is required and must be 1..63 characters";
                return {};
            }
            for (const char ch: cfg.position_name) {
                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
                    error = tag + ".id: only [A-Za-z0-9_-] allowed (used in filenames)";
                    return {};
                }
            }
            if (!ids.insert(cfg.position_name).second) {
                error = "lidar: duplicate id \"" + cfg.position_name + "\"";
                return {};
            }

            bool enabled = true;
            if (item["enabled"]) {
                enabled = item["enabled"].as<bool>();
            }
            if (!enabled) {
                g_log.trace("LiDAR instance [{}] skipped (disabled)", cfg.position_name);
                continue;
            }

            if (auto d = item["device"]) {
                if (d["ip"]) {
                    cfg.hostname = d["ip"].as<std::string>();
                }
                if (d["port"]) {
                    cfg.driver_config.device.port = d["port"].as<std::uint16_t>();
                }
            }
            if (cfg.hostname.empty()) {
                error = tag + ".device.ip is required";
                return {};
            }

            // Apply shared config
            cfg.driver_config.device.ip = cfg.hostname;
            cfg.driver_config.scan = shared_scan;
            cfg.driver_config.network = shared_network;
            cfg.enable_ntp = shared_enable_ntp;
            cfg.ntp_server_ip = shared_ntp_server;
            cfg.sync_time = sync_interval_s;
            cfg.ntp_check_status_s = check_status_s;
            cfg.recording_queue_capacity = shared_queue_max_frames;
            cfg.recording_write_buffer_size = shared_write_buffer_bytes;
            cfg.recording_max_file_bytes = shared_max_file_bytes;
            cfg.stats_interval_s = stats_interval_s;

            g_log.trace("Loaded LiDAR instance [{}] ({}:{})", cfg.position_name, cfg.hostname,
                        cfg.driver_config.device.port);

            configs.push_back(std::move(cfg));
        }

        if (configs.empty()) {
            error = "LiDARs: at least one LiDAR must be enabled";
            return {};
        }

        g_log.trace("Loaded {} LiDAR instance(s)", configs.size());
        } catch (const std::exception &e) {
            // ConfigLoader / YAML conversion failures propagate as text too
            error = e.what();
            return {};
        }
        return configs;
    }
} // namespace LMS4xxxTool
