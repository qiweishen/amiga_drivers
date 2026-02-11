#include "tool.h"

#include <boost/crc.hpp>
#include <vector>


namespace Tool {
    namespace Utility {
        std::vector<std::string_view> SplitString(std::string_view str, char delimiter) {
            std::vector<std::string_view> tokens;
            size_t start = 0;
            size_t end = str.find(delimiter);
            while (end != std::string_view::npos) {
                tokens.push_back(str.substr(start, end - start));
                start = end + 1;
                end = str.find(delimiter, start);
            }
            if (start <= str.length()) {
                tokens.push_back(str.substr(start));
            }
            return tokens;
        }
    } // namespace Utility

    void LogMessage(const spdlog::level::level_enum level, const std::string_view module, const std::string_view msg,
                    const std::string_view error) {
        if ((level == spdlog::level::trace || level == spdlog::level::info) && !error.empty()) {
            spdlog::warn("[{}] {}: Error message provided for trace/info level", module);
        }
        if (error.empty()) {
            spdlog::log(level, "[{}] {}: {}", module, msg);
        } else {
            spdlog::log(level, "[{}] {}: {} - {}", module, msg, error);
        }
        if (level == spdlog::level::err || level == spdlog::level::critical) {
            throw std::runtime_error("Error Detected");
        }
    }
} // namespace Tool
