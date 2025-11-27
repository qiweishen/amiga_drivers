#pragma once

#include <spdlog/spdlog.h>
#include <vector>



namespace Tool {

	namespace Utility {
		/**
		 * @brief Split a string view by a single character delimiter.
		 *
		 * Returns lightweight string_view slices that reference the original input,
		 * avoiding additional allocations.
		 *
		 * @param str       Input string view to split.
		 * @param delimiter Character used to split the string.
		 * @return Vector of string_view tokens.
		 */
		std::vector<std::string_view> SplitString(std::string_view str, char delimiter);

	}  // namespace Utility

	/**
	 * @brief Log a message with optional error details using spdlog.
	 *
	 * @param level spdlog severity level.
	 * @param module Name of the module emitting the log.
	 * @param func Name of the function emitting the log.
	 * @param msg Primary log message.
	 * @param error (Optional) Error message or diagnostic context.
	 */
	void LogMessage(spdlog::level::level_enum level, std::string_view module, std::string_view func, std::string_view msg,
					std::string_view error = "");

}  // namespace Tool
