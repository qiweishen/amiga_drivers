#include "tool.h"

#include <filesystem>
#include <INIReader.h>
#include <stdexcept>
#include <spdlog/fmt/chrono.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>


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
                LogMessage(spdlog::level::err, kModule, "Latitude / Longitude must be in radians and within valid ranges");
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

        std::vector<std::string> SplitString(std::string_view str, char delimiter) {
            std::vector<std::string> tokens;
            size_t start = 0;
            size_t end = str.find(delimiter);
            while (end != std::string_view::npos) {
                tokens.emplace_back(str.substr(start, end - start));
                start = end + 1;
                end = str.find(delimiter, start);
            }
            if (start <= str.size()) {
                tokens.emplace_back(str.substr(start));
            }
            return tokens;
        }
    } // namespace Utility


    void InitializeSystem(Config &config) {
        // Prepare data directory
        auto now = std::chrono::system_clock::now();
        config.timestamp = fmt::format("{:%Y%m%d_%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(now));
        config.data_folder_path = fmt::format("{}/{}", config.output_directory, config.timestamp);
        std::filesystem::create_directories(config.data_folder_path);

        // Prepare logger with both console and file sinks
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::info);
        console_sink->set_pattern("%^[%H:%M:%S] [%l] %v%$");
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            fmt::format("{}/log_{}.log", config.data_folder_path, config.timestamp), true);
        file_sink->set_level(spdlog::level::trace);
        file_sink->set_pattern("[%H:%M:%S] [%l] %v");
        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
        auto logger = std::make_shared<spdlog::logger>("INS401 Driver", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::trace);
        spdlog::set_default_logger(logger);
    }


    void LoadConfig(std::string_view config_path, Config &config) {
        constexpr std::string_view kModule = "Tool::LoadConfig";

        const INIReader configures(std::string{config_path});
        if (configures.ParseError() < 0) {
            LogMessage(spdlog::level::err, kModule, fmt::format("Cannot load Config.ini file from {}", config_path));
            throw std::runtime_error("Failed to load configuration file");
        }
        config.output_directory = configures.Get("General", "Output Directory", "./data");
        config.enable_logging = configures.GetBoolean("Logging System", "Enable Logging", true);
        config.enable_rtk = configures.GetBoolean("NTRIP Client", "Enable RTK", false);
        config.host = configures.Get("NTRIP Client", "Host", "localhost");
        config.port = static_cast<int>(configures.GetInteger("NTRIP Client", "Port", 2101));
        config.mount_point = configures.Get("NTRIP Client", "Mount Point", "MOUNT");
        config.use_vrs = configures.GetBoolean("NTRIP Client", "Use VRS", false);
        config.username = configures.Get("NTRIP Client", "Username", "user");
        config.password = configures.Get("NTRIP Client", "Password", "password");
        config.enable_gnss_checking = configures.GetBoolean("Static Initialization", "Enable GNSS Checking", false);
        config.gnss_horizontal_std_threshold = configures.GetReal("Static Initialization", "GNSS Horizontal STD", 0.03);
        config.accel_gravity_threshold = configures.GetReal("Static Initialization", "Accel Gravity Threshold", 0.035);
        config.accel_var_threshold = configures.GetReal("Static Initialization", "Accel Variance Threshold", 0.008);
        config.gyro_var_threshold = configures.GetReal("Static Initialization", "Gyro Variance Threshold", 0.125);
        config.gyro_mean_threshold_xy = configures.GetReal("Static Initialization", "Gyro Mean Threshold_xy", 0.035);
        config.gyro_mean_threshold_z = configures.GetReal("Static Initialization", "Gyro Mean Threshold_z", 0.125);
        config.min_stationary_duration_s = configures.GetReal("Static Initialization", "Minimal Stationary Duration",
                                                              10);
        config.recompute_interval_s = configures.GetReal("Static Initialization", "Recompute Interval", 5);
        config.required_stable_count = static_cast<int>(configures.GetInteger(
            "Static Initialization", "Required Stable Count", 5));
        config.stability_threshold_deg = configures.GetReal("Static Initialization",
                                                            "Stability Initialization Threshold deg", 0.1);
    }

    void LogMessage(spdlog::level::level_enum level, std::string_view module, std::string msg,
                    std::string error) {
        if (error.empty()) {
            spdlog::log(level, "[{}]: {}", module, msg);
        } else {
            spdlog::log(level, "[{}]: {} - {}", module, msg, error);
        }
        // Fatal by design: err/critical halt the program. Callers needing non-fatal
        // error handling should catch std::runtime_error.
        if (level == spdlog::level::err || level == spdlog::level::critical) {
            throw std::runtime_error("Error Detected, check logs for details");
        }
    }
} // namespace Tool
