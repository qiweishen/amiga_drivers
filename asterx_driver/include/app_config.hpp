#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "commands.hpp"

namespace asterx {
    class ConfigLoadError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct AppConfig {
        std::string host{"10.95.76.111"};
        // Single TCP connection: ASCII commands and the SBF stream share it
        std::uint16_t ctrl_port{28784};
        std::string user{"admin"};
        std::string password{"septentrio"};

        std::filesystem::path output_dir{"./recordings"};
        std::string file_prefix{"asterx"};
        std::uint64_t rotate_bytes{1ull << 30};
        int rotate_interval_seconds{3600};

        // Log-level filter for the shim; output goes to the unified spdlog logger
        std::string log_level{"info"};

        ReceiverSettings receiver{};
    };

    [[nodiscard]] AppConfig load_app_config(const std::string &path);
} // namespace asterx
