#include "tool.h"

// #include <GeographicLib/NormalGravity.hpp>
// #include <GeographicLib/LocalCartesian.hpp>
#include <Eigen/Core>
#include <vector>


namespace Tool {
    // namespace Earth {
    //     double ComputeGravity(double latitude, double longitude, double height) {
    //         Eigen::Vector3d geodetic_pos{latitude, longitude, height};
    //         GeographicLib::Geocentric geocentric_model(GeographicLib::Geocentric::WGS84());
    //         GeographicLib::NormalGravity gravity_model(GeographicLib::NormalGravity::WGS84());
    //         double x, y, z;
    //         std::vector<double> M(9);
    //         geocentric_model.Forward(geodetic_pos(0), geodetic_pos(1), geodetic_pos(2), x, y, z, M);
    //
    //         double gx, gy, gz;
    //         gravity_model.U(x, y, z, gx, gy, gz);
    //
    //         // M maps local ENU -> ECEF (row-major). Use transpose for ECEF -> ENU.
    //         const Eigen::Matrix3d R_local_to_ecef = Eigen::Map<const Eigen::Matrix<double, 3, 3,
    //             Eigen::RowMajor>>(M.data());
    //         Eigen::Vector3d g_ecef(gx, gy, gz);
    //
    //         Eigen::Vector3d g_enu_raw = R_local_to_ecef.transpose() * g_ecef;
    //         Eigen::Vector3d gravity_enu;
    //         // Ensure the gravity vector points upward (positive Up / Z component in ENU).
    //         // GeographicLib's U() gradient points toward Earth center (downward, negative Z in ENU).
    //         if (g_enu_raw(2) < 0.0) {
    //             gravity_enu = -g_enu_raw;
    //         } else {
    //             gravity_enu = g_enu_raw;
    //         }
    //         return gravity_enu.norm();
    //     }
    // }
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
            spdlog::warn("[{}]: Error message provided for trace/info level", module);
        }
        if (error.empty()) {
            spdlog::log(level, "[{}]: {}", module, msg);
        } else {
            spdlog::log(level, "[{}]: {} - {}", module, msg, error);
        }
        if (level == spdlog::level::err || level == spdlog::level::critical) {
            throw std::runtime_error("Error Detected");
        }
    }
} // namespace Tool
