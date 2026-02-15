#include "tool.h"


namespace Tool {
    namespace Earth {
        constexpr std::string_view kModule = "Tool::Earth";

        // Here we use the WGS84 ellipsoid model to compute gravity based on latitude, longitude, and height.
        // To distinguish from other LLH (deg, deg, m) formats, we use BLH (rad, rad, m) for the input.
        // BLH: Breite (Latitude, radians), Länge (Longitude, radians), Höhe (Height, meters).
        // Returned gravity is **POSITIVE** in m/s^2.
        double ComputeGravity(const Eigen::Vector3d &blh) {
            // Check for valid latitude and longitude ranges.
            // Although longitude can be any value since it doesn't affect gravity, we still check it.
            if (blh[0] < -M_PI / 2 || blh[0] > M_PI / 2 || blh[1] < -M_PI || blh[1] > M_PI) {
                LogMessage(spdlog::level::err, kModule, "Latitude / Longitude must in radians and within valid ranges");
            }
            // Following formula adapted from: https://github.com/i2Nav-WHU/KF-GINS/blob/main/src/common/earth.h
            double sinphi = sin(blh[0]);
            double sin2 = sinphi * sinphi;
            double sin4 = sin2 * sin2;
            // Normal gravity at equator
            double gamma_a = 9.7803267715;
            // Series expansion of normal gravity at given latitude
            double gamma_0 = gamma_a * (1 + 0.0052790414 * sin2 + 0.0000232718 * sin4 + 0.0000001262 * sin2 * sin4 +
                                        0.0000000007 * sin4 * sin4);
            // Changes of normal gravity with height
            double gamma = gamma_0 - (3.0877e-6 - 4.3e-9 * sin2) * blh[2] + 0.72e-12 * blh[2] * blh[2];
            return gamma;
        }
    } // namespace Earth
    namespace Utility {
        constexpr std::string_view kModule = "Tool::Utility";

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

    void LogMessage(const spdlog::level::level_enum level, const std::string_view module,
                    const std::string_view msg,
                    const std::string_view error) {
        if ((level == spdlog::level::trace || level == spdlog::level::info) && !error.empty()) {
            spdlog::warn("[{}]: Error message provided for trace/info level", module);
        }
        if (error.empty()) {
            spdlog::log(level, "[{}]: {}", module, msg);
        } else {
            spdlog::log(level, "[{}]: {} - {}", module, msg, error);
        }
        if (level == spdlog::level::err || level == spdlog::level::critical) {
            throw std::runtime_error("Error Detected, check above logs for details");
        }
    }
} // namespace Tool
