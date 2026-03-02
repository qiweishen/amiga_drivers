#include "lidar_tool.h"


namespace LidarTool {
    void LoadConfig(std::string_view config_path, LiDARConfig &config) {
        Common::ConfigLoader loader(config_path);
        const auto &root = loader.root();

        // SICK Launch
        if (root["Sick Launch"]) {
            const auto &launch = root["Sick Launch"];
            config.launch_file = launch["SICK Launch file"].as<std::string>(config.launch_file);
            config.lidar_ip = launch["hostname"].as<std::vector<std::string> >(config.lidar_ip);
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
}
