#ifndef COMMON_DATA_TYPE_H
#define COMMON_DATA_TYPE_H

namespace Common {
    struct Config {
        std::string output_directory;
        int run_mode;
        std::string ins_config_path;
        std::string lidar_config_path;
        std::string lidar_launch_path;
        bool enable_logging{};

        std::string timestamp;
        std::string data_folder_path;
    };
};

#endif //COMMON_DATA_TYPE_H