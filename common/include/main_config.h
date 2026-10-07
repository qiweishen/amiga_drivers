#pragma once

#include <filesystem>
#include <yaml-cpp/yaml.h>

#include "data_type.h"

namespace common {
    // Parse into a fresh value: an invalid field cannot leave a partly applied
    // configuration. Relative paths use the project root, not the YAML folder.
    // This does not create files, initialize logging or access any device.
    Config ParseMainConfig(const YAML::Node &root, const std::filesystem::path &project_root);

    Config LoadMainConfig(const std::filesystem::path &config_path,
                          const std::filesystem::path &project_root);
} // namespace common
