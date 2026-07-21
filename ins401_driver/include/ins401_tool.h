/// @file ins401_tool.h
/// @brief System initialization, configuration loading, gravity computation, and string utilities.

#ifndef INS401_TOOL_H
#define INS401_TOOL_H

#include <Eigen/Core>
#include <string_view>
#include <vector>

#include "ins401_data_type.h"


namespace INS401::Tool {
	namespace Earth {
		double ComputeGravity(const Eigen::Vector3d &blh);
	}  // namespace Earth

	namespace Utility {
		std::vector<std::string> SplitString(std::string_view str, char delimiter);
	}  // namespace Utility

	void LoadConfig(std::string_view config_path, INSConfig &config);
}  // namespace INS401::Tool


#endif
