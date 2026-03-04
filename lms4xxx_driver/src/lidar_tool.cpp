#include "lidar_tool.h"

#include <cctype>


namespace {
    // "Front Left Laser" -> "front_left_laser"
    std::string ToSnakeCase(const std::string& name) {
        std::string result;
        for (char c : name) {
            result += (c == ' ') ? '_' : static_cast<char>(std::tolower(c));
        }
        return result;
    }
}


namespace LidarTool {
    void LoadConfig(std::string_view config_path, LiDARConfig &config) {
        Common::ConfigLoader loader(config_path);
        const auto &root = loader.root();

        // SICK Launch
        if (root["Sick Launch"]) {
            const auto &launch = root["Sick Launch"];
            config.launch_file = launch["SICK Launch file"].as<std::string>(config.launch_file);
            config.lidar_ip = launch["hostname"].as<std::string>(config.lidar_ip);
            if (!config.lidar_ip.empty()) {
                config.launch_overrides["hostname"] = config.lidar_ip;
            }
        }

        // Time Synchronization
        if (root["Time Synchronization"]) {
            config.ntp_server_ip = root["Time Synchronization"]["NTP Server"].as<std::string>("");
        }

        // General (standalone main config)
        if (root["General"]) {
            const auto &general = root["General"];
            if (general["Output Directory"]) {
                config.data_folder_path = general["Output Directory"].as<std::string>("./data");
            }
        }

        // Logging System (standalone main config)
        if (root["Logging System"]) {
            bool enable = root["Logging System"]["Enable Logging"].as<bool>(true);
            if (!enable) {
                config.log_file.clear();
            }
        }
    }


    std::vector<LiDARConfig> LoadConfigs(std::string_view config_path) {
        Common::ConfigLoader loader(config_path);
        const auto &root = loader.root();

        std::string launch_file;
        std::map<std::string, std::string> shared_overrides;
        std::vector<LiDARConfig> configs;

        // Parse "Sick Launch" section
        if (root["Sick Launch"]) {
            const auto &launch = root["Sick Launch"];

            for (auto it = launch.begin(); it != launch.end(); ++it) {
                const auto key = it->first.as<std::string>();
                auto value = it->second;

                if (key == "SICK Launch file") {
                    // Shared launch file path
                    launch_file = value.as<std::string>();
                } else if (value.IsMap() && value["hostname"]) {
                    // Per-LiDAR instance node
                    LiDARConfig cfg;
                    cfg.lidar_position = ToSnakeCase(key);
                    cfg.lidar_ip = value["hostname"].as<std::string>();

                    // Collect all key-value pairs as launch overrides
                    for (auto sub = value.begin(); sub != value.end(); ++sub) {
                        const auto sub_key = sub->first.as<std::string>();
                        cfg.launch_overrides[sub_key] = sub->second.as<std::string>();
                    }
                    configs.push_back(std::move(cfg));
                } else if (value.IsScalar()) {
                    // Shared override (e.g. tick_to_timestamp_mode: 2)
                    shared_overrides[key] = value.as<std::string>();
                }
            }
        }

        // Apply shared fields to all instances
        std::string ntp_server_ip;
        if (root["Time Synchronization"]) {
            ntp_server_ip = root["Time Synchronization"]["NTP Server"].as<std::string>("");
        }

        bool logging_enabled = true;
        if (root["Logging System"]) {
            logging_enabled = root["Logging System"]["Enable Logging"].as<bool>(true);
        }

        for (auto &cfg : configs) {
            cfg.launch_file = launch_file;
            cfg.ntp_server_ip = ntp_server_ip;

            if (!logging_enabled) {
                cfg.log_file.clear();
            }

            // Merge shared overrides (per-LiDAR values take priority)
            for (const auto &[key, value] : shared_overrides) {
                if (cfg.launch_overrides.find(key) == cfg.launch_overrides.end()) {
                    cfg.launch_overrides[key] = value;
                }
            }
        }

        return configs;
    }
}
