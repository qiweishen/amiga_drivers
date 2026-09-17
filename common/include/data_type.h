#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "run_guards.h"


namespace common {
    class SensorSyncHub;

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

        // The rig's one SensorSync-Logger board: its serial port ("Sensor Trigger: Port" in
        // config-main.yaml; "" = no board) and the session shared by every camera it
        // triggers (created by main for the run; see common/include/sensor_sync_hub.h)
        std::string sensor_trigger_port;
        std::shared_ptr<SensorSyncHub> sensor_sync;
    };
} // namespace common
