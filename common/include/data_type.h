#pragma once

#include <string>
#include <filesystem>

#include "run_guards.h"


namespace common {
    struct Config {
        std::string operator_name;
        std::string field_name;

        std::filesystem::path output_directory;

        bool enable_asterx{false};
        bool enable_fx10{false};
        bool enable_gox{false};
        bool enable_lms4xxx{false};

        std::filesystem::path asterx_config_path;
        std::filesystem::path fx10_config_path;
        std::filesystem::path gox_config_path;
        std::filesystem::path lms4xxx_config_path;

        // Rig-wide disk floor and no-data watchdog, enforced by main for every
        // enabled driver (see common/include/run_guards.h)
        GuardConfig guards;

        std::string timestamp;
        std::filesystem::path data_folder_path;
    };
} // namespace common
