#pragma once

#include <filesystem>
#include <string>

#include "app_config.h" // fx10::ConfigError


// Pre-load checks for the eBUS Player file (.pvcfg) restored right after factory
// preparation. Layout: <dir named after DeviceSerialNumber>/<file>.pvcfg next to
// checksums.txt (sha256sum format). SDK-free; CameraControl loads the file as is.
namespace fx10 {
    // Lowercase hex SHA-256 of a file (OpenSSL EVP). Throws ConfigError.
    std::string Sha256File(const std::filesystem::path &path);

    // Throws ConfigError unless the parent directory name equals the trimmed serial.
    void CheckEbusConfigSerial(const std::filesystem::path &pvcfg, const std::string &device_serial);

    // Requires exactly one checksums.txt entry for the file name and a matching
    // SHA-256. Returns the verified digest. Throws ConfigError.
    std::string CheckEbusConfigChecksum(const std::filesystem::path &pvcfg);
} // namespace fx10
