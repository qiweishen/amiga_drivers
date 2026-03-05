#ifndef LIDAR_TOOL_H
#define LIDAR_TOOL_H

#include <string>
#include <string_view>
#include <vector>

#include "lms4xxx_data_type.h"


namespace LMS4xxxTool {
	// Load multiple LiDARConfig instances from a unified YAML config file
	std::vector<LiDARConfig> LoadConfigs(std::string_view config_path);

	// Convert a position name to snake_case (e.g., "Front Left" -> "front_left")
	std::string ToSnakeCase(std::string_view name);

}  // namespace LidarTool

#endif	// LIDAR_TOOL_H
