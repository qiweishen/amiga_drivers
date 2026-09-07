#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "commands.h"
#include "config_util.h"


namespace asterx {
    struct AppConfig {
        std::string host;
        std::uint16_t ctrl_port{28784}; // one TCP connection carries commands and the SBF stream

        std::filesystem::path output_dir; // injected by the host, never from the yaml
        std::string file_prefix{"asterx"};
        std::uint64_t rotate_bytes{1ull << 30};
        int rotate_interval_seconds{3600};
        // SbfWriteQueue budget: how far the disk may fall behind before the run ends
        std::uint64_t write_queue_bytes{256ull << 20};
        bool live_csv{true}; // per-block live_*.csv beside the .sbf (GUI live view)

        int stats_period_ms{2500};

        ReceiverSettings receiver{}; // credentials come from device.user / device.password
    };

    // Strict schema: unknown keys are errors; device.{ip,user,password} and receiver.sbf_streams are
    // required, everything else keeps the defaults. Throws ConfigError.
    AppConfig LoadAppConfig(const std::string &path);

    AppConfig LoadAppConfigText(const std::string &yaml_text);
} // namespace asterx
