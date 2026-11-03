#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>


namespace lms4xxx {
    struct RawRecord;


    enum class ConnectionState : std::uint8_t {
        kDisconnected = 0,
        kConnecting = 1,
        kConnected = 2,
        kConfiguring = 3,
        kScanning = 4,
        kError = 5,
    };


    // Per complete CoLa B frame, including control replies and checksum failures.
    // Runs on the receive thread. Move into an owning queue; do not block or throw.
    using TelegramCallback = std::function<void(RawRecord &&record)>;

    // Must be thread-safe
    using ErrorCallback = std::function<void(std::error_code ec, const std::string &detail)>;
} // namespace lms4xxx
