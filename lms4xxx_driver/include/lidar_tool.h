#ifndef LIDAR_TOOL_H
#define LIDAR_TOOL_H

#include "lidar_data_type.h"
#include "utility.h"


namespace LidarTool {
    void LoadConfig(std::string_view config_path, LiDARConfig &config);
}


#endif