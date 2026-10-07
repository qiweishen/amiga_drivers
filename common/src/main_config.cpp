#include "main_config.h"

#include "config_util.h"
#include "utility.h"

namespace common {
    Config ParseMainConfig(const YAML::Node &root, const std::filesystem::path &project_root) {
        using namespace ConfigUtil;
        if (!root.IsDefined() || !root.IsMap()) Fail("document", "must be a mapping");
        CheckKeys(root, "", {"General", "Guards", "Sensor Trigger"});
        const YAML::Node general = RequireMap(root, "", "General", {
            "Operator", "Field", "Output Directory", "Enable ASTERX", "Enable FX10", "Enable GOX",
            "Enable LMS4XXX", "ASTERX Driver Config Path", "FX10 Driver Config Path",
            "GOX Driver Config Path", "LMS4XXX Driver Config Path"
        });

        Config config;
        config.operator_name = "Anonymous";
        config.field_name = "Unknown";
        ReadText(general, "General", "Operator", config.operator_name);
        ReadText(general, "General", "Field", config.field_name);

        // Omitted switches remain false for every device. A missing/invalid
        // switch must never accidentally start hardware or hide a typo.
        Read(general, "General", "Enable ASTERX", config.enable_asterx);
        Read(general, "General", "Enable FX10", config.enable_fx10);
        Read(general, "General", "Enable GOX", config.enable_gox);
        Read(general, "General", "Enable LMS4XXX", config.enable_lms4xxx);

        const auto path = [&](std::string_view key, std::string value) {
            ReadText(general, "General", key, value);
            const std::filesystem::path input(value);
            return GetAbsolutePath(input.is_relative() ? project_root / input : input);
        };
        config.output_directory = path("Output Directory", "./data");
        config.asterx_config_path = path("ASTERX Driver Config Path", "./asterx_driver/config/config-asterx.yaml");
        config.fx10_config_path = path("FX10 Driver Config Path", "./fx10_driver/config/config-fx10.yaml");
        config.gox_config_path = path("GOX Driver Config Path", "./gox_driver/config/config-gox.yaml");
        config.lms4xxx_config_path = path("LMS4XXX Driver Config Path", "./lms4xxx_driver/config/config-lms4xxx.yaml");

        const YAML::Node guards = OptionalMap(root, "", "Guards", {
            "Disk Min Free GiB", "Disk Warn Free GiB", "No Data Warn S", "No Data Abort S"
        });
        ReadRange(guards, "Guards", "Disk Min Free GiB", 0.0, 86400.0, config.guards.disk_min_free_gib);
        ReadRange(guards, "Guards", "Disk Warn Free GiB", 0.0, 86400.0, config.guards.disk_warn_free_gib);
        ReadRange(guards, "Guards", "No Data Warn S", 0.0, 86400.0, config.guards.no_data_warn_s);
        ReadRange(guards, "Guards", "No Data Abort S", 0.0, 86400.0, config.guards.no_data_abort_s);
        if (config.guards.disk_warn_free_gib > 0.0 &&
            config.guards.disk_warn_free_gib <= config.guards.disk_min_free_gib) {
            Fail("Guards.Disk Warn Free GiB", "must be greater than Disk Min Free GiB (or 0 to disable the warning)");
        }
        if (config.guards.no_data_abort_s > 0.0 && config.guards.no_data_warn_s >= config.guards.no_data_abort_s) {
            Fail("Guards.No Data Warn S", "must be smaller than No Data Abort S (or 0 to disable the warning)");
        }

        const YAML::Node trigger = OptionalMap(root, "", "Sensor Trigger", {"Port"});
        // An explicit empty string disables the board; null and malformed
        // values are errors, just like other present scalar fields.
        Read(trigger, "Sensor Trigger", "Port", config.sensor_trigger_port);
        if (!config.sensor_trigger_port.empty() &&
            !std::filesystem::path(config.sensor_trigger_port).is_absolute()) {
            Fail("Sensor Trigger.Port", "must be an absolute device path (or an empty string to disable)");
        }
        return config;
    }

    Config LoadMainConfig(const std::filesystem::path &config_path,
                          const std::filesystem::path &project_root) {
        const auto root = ConfigUtil::LoadFile(config_path.string());
        try {
            return ParseMainConfig(root, project_root);
        } catch (const ConfigError &e) {
            throw ConfigError("config '" + config_path.string() + "': " + e.what());
        }
    }
} // namespace common
