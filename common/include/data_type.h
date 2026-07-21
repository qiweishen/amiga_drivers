#ifndef COMMON_DATA_TYPE_H
#define COMMON_DATA_TYPE_H

#include <string>


namespace Common {
    struct Config {
        std::string output_directory;

        bool enable_asterx{false};
        bool enable_gox{false};
        bool enable_ins401{true};
        bool enable_lms4xxx{true};

        std::string asterx_config_path;
        std::string gox_config_path;
        std::string ins401_config_path;
        std::string lms4xxx_config_path;
        bool enable_logging{};

        std::string timestamp;
        std::string data_folder_path;
    };
} // namespace Common

#endif //COMMON_DATA_TYPE_H
