#ifndef LMS4XXX_TOOL_H
#define LMS4XXX_TOOL_H

#include <string>
#include <string_view>
#include <vector>

#include "lms4xxx_data_type.h"


namespace LMS4xxxTool {
    // Load the enabled LiDAR instances from the unified YAML config file.
    // NEVER throws (this layer only propagates): on any config problem `error`
    // is filled with the concrete reason and an empty vector is returned — the
    // caller (main) owns the abort decision.
    std::vector<LiDARConfig> LoadConfigs(std::string_view config_path, std::string &error);

    // Convert a position name to snake_case (e.g., "Front Left" -> "front_left")
    std::string ToSnakeCase(std::string_view name);
} // namespace LMS4xxxTool

#endif	// LMS4XXX_TOOL_H
