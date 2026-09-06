#pragma once

// SDK-free helper for the factory-default load (docs/DEVICE_CONFIG.md).

#include <optional>
#include <string>
#include <vector>

namespace gox {
    // The UserSetSelector entry spelled as the camera reports it whose name is
    // "default" (case-insensitive); nullopt when there is none. Only that name
    // is accepted — no "Factory"/"UserSet0" guessing.
    std::optional<std::string> PickDefaultUserSetEntry(const std::vector<std::string> &entries);
} // namespace gox
