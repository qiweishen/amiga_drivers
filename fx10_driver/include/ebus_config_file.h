#pragma once

#include <cstdint>
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

    // Image geometry in the camera's node units: Width/Height are binned pixels.
    struct SensorGeometry {
        std::int64_t width = 0;
        std::int64_t height = 0;
        std::int64_t offset_x = 0;
        std::int64_t offset_y = 0;
        std::int64_t binning_horizontal = 0;
        std::int64_t binning_vertical = 0;

        // Detector columns / rows covered by the image.
        [[nodiscard]] std::int64_t WindowColumns() const { return width * binning_horizontal; }
        [[nodiscard]] std::int64_t WindowRows() const { return height * binning_vertical; }

        bool operator==(const SensorGeometry &) const = default;
    };

    // "Width=1024 Height=224 OffsetX=112 OffsetY=285 BinningHorizontal=1 BinningVertical=2 (window 1024x448)"
    std::string FormatSensorGeometry(const SensorGeometry &geometry);

    // Width, Height and binning in 1..2^20, offsets in 0..2^20. Throws ConfigError naming `what`.
    void ValidateSensorGeometry(const SensorGeometry &geometry, const std::string &what);

    // Same detector window: equal offsets and equal detector columns/rows, at any binning.
    [[nodiscard]] bool SameSensorWindow(const SensorGeometry &a, const SensorGeometry &b);

    // Width/Height/OffsetX/OffsetY/BinningHorizontal/BinningVertical from the
    // file's single device parameter section (<device ...> ... <device> here </device>).
    // Each must appear there exactly once as a decimal integer and pass
    // ValidateSensorGeometry. Throws ConfigError naming `what`.
    SensorGeometry ParseEbusConfigGeometry(const std::string &pvcfg_text, const std::string &what);

    SensorGeometry ReadEbusConfigGeometry(const std::filesystem::path &pvcfg);
} // namespace fx10
