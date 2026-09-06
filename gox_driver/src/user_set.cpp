#include "user_set.h"

#include <algorithm>
#include <cctype>

namespace gox {
    std::optional<std::string> PickDefaultUserSetEntry(const std::vector<std::string> &entries) {
        for (const auto &entry: entries) {
            std::string lower = entry;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "default") {
                return entry;
            }
        }
        return std::nullopt;
    }
} // namespace gox
