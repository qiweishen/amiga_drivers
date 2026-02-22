/// @file tool.h
/// @brief System initialization, configuration loading, gravity computation, and logging utilities.

#ifndef TOOL_H
#define TOOL_H

#include <spdlog/spdlog.h>
#include <string_view>
#include <vector>
#include <Eigen/Core>

#include "data_type.h"


namespace Tool {
    namespace Earth {
        double ComputeGravity(const Eigen::Vector3d &blh);
    } // namespace Earth

    namespace Utility {
        std::vector<std::string> SplitString(std::string_view str, char delimiter);
    } // namespace Utility

    void InitializeSystem(Config &config);

    void LoadConfig(std::string_view config_path, Config &config);

    /// Log message with optional error details.
    /// WARNING: levels `err` and `critical` throw std::runtime_error after logging.
    void LogMessage(spdlog::level::level_enum level, std::string_view module, std::string msg,
                    std::string error = "");
} // namespace Tool


#endif