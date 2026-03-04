#ifndef LIDAR_TOOL_H
#define LIDAR_TOOL_H

#include <vector>

#include "lidar_data_type.h"
#include "utility.h"


namespace LidarTool {
    // Load a single config for standalone mode
    void LoadConfig(std::string_view config_path, LiDARConfig &config);

    // Parse YAML and return one LiDARConfig per LiDAR instance (unified main)
    std::vector<LiDARConfig> LoadConfigs(std::string_view config_path);
}


#endif