#ifndef TOOL_H
#define TOOL_H

#include <spdlog/spdlog.h>
#include <vector>

#include "Eigen/Core"


namespace Tool {
    namespace Earth {
        double ComputeGravity(const Eigen::Vector3d &blh);
    } // namespace Earth

    namespace Utility {
        // String splitting without allocations.
        std::vector<std::string_view> SplitString(std::string_view str, char delimiter);
    } // namespace Utility

    // Log message with optional error details.
    void LogMessage(spdlog::level::level_enum level, std::string_view module, std::string_view msg,
                    std::string_view error = "");
} // namespace Tool


#endif