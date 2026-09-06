#pragma once
#include <functional>
#include <string>
#include <system_error>


namespace lms4xxx {
    struct ScanData;


    enum class ConnectionState : std::uint8_t {
        kDisconnected = 0,
        kConnecting = 1,
        kConnected = 2,
        kConfiguring = 3,
        kScanning = 4,
        kError = 5,
    };


    // Per complete scan frame; must not block or throw
    using ScanDataCallback = std::function<void(const ScanData &scan_data)>;

    // Must be thread-safe
    using ErrorCallback = std::function<void(std::error_code ec, const std::string &detail)>;
} // namespace lms4xxx

