/// @file lidar_driver_app.cpp
/// @brief LidarDriverApp lifecycle implementation
///
/// Unified and standalone main paths both flow through init/run/shutdown
/// The receiver (LidarReceiver) owns the SICK API, queue, writer, and statistics

#include "lidar_driver_app.h"

#include <chrono>
#include <spdlog/spdlog.h>

#include "csv_converter.h"
#include "lidar_receiver.h"
#include "lidar_tool.h"
#include "utility.h"


namespace {
    constexpr std::string_view kModule = "LidarDriverApp";
}


LidarDriverApp::LidarDriverApp(const Common::Config &config) {
    // Store config paths from the main config. Actual loading deferred to init()
    config_path_ = config.lidar_config_path;
    launch_file_override_ = config.lidar_launch_path;
    data_folder_path_ = config.data_folder_path;
    timestamp_ = config.timestamp;
}


LidarDriverApp::LidarDriverApp(LiDARConfig config) : config_preloaded_(true), config_(std::move(config)) {
}


LidarDriverApp::~LidarDriverApp() {
    shutdown();
}


bool LidarDriverApp::init() {
    // Load config from YAML if not preloaded (unified main path)
    if (!config_preloaded_) {
        if (config_path_.empty()) {
            std::filesystem::path exe_dir = Common::GetExecutableDir();
            config_path_ = (exe_dir / "../../lms4xxx_driver/config/config-lms4xxx.yaml").string();
        }

        try {
            LidarTool::LoadConfig(config_path_, config_);
        } catch (const std::exception &e) {
            Common::Log::log_message(spdlog::level::warn, kModule,
                                     fmt::format("Config load failed: {}", e.what()));
            return false;
        }

        // Override fields from unified config
        if (!launch_file_override_.empty()) {
            config_.launch_file = launch_file_override_;
        }
        config_.data_folder_path = data_folder_path_;
        config_.timestamp = timestamp_;
    }

    // Compute output file path if not explicitly set
    if (config_.output_file.empty() && !config_.data_folder_path.empty()) {
        config_.output_file = fmt::format("{}/pointcloud_{}_{}.bin", config_.data_folder_path, config_.lidar_position, config_.timestamp);
    }

    // Suppress SICK console output in unified mode (logging goes through shared logger)
    if (!config_preloaded_) {
        config_.quiet = true;
    }

    // Create and initialize the receiver
    receiver_ = std::make_unique<LidarReceiver>(config_);
    if (!receiver_->Init()) {
        return false;
    }
    if (!receiver_->Start()) {
        return false;
    }

    Common::Log::log_message(spdlog::level::info, kModule,
                             fmt::format("LiDAR [{}] initialized", config_.lidar_position));
    return true;
}


void LidarDriverApp::run() {
    while (!terminate_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}


void LidarDriverApp::shutdown() {
    if (shutdown_called_.exchange(true)) return;

    // 1. Signal termination
    terminate_.store(true, std::memory_order_release);

    // 2. Stop receiver (deregisters callbacks, drains queue, closes writer, releases API)
    if (receiver_) {
        receiver_->Stop();
        receiver_->LogStatistics();
    }

    // 3. Optional post-processing: binary→CSV conversion (matching INS401 pattern)
    if (config_.convert_to_csv && receiver_ && !config_.output_file.empty()) {
        std::string csv_path = BinaryToCsvConverter::default_csv_path(config_.output_file);
        Common::Log::log_message(spdlog::level::info, kModule,
                                 fmt::format("Converting {} -> {}", config_.output_file, csv_path));

        BinaryToCsvConverter converter;
        converter.set_progress_callback([](uint64_t frames, uint64_t bytes, uint64_t total) {
            double pct = total > 0 ? (100.0 * static_cast<double>(bytes) / static_cast<double>(total)) : 0.0;
            fmt::print("\r[Converter] Frames: {}  Progress: {}%", frames, static_cast<int>(pct));
            std::fflush(stdout);
        });

        auto result = converter.convert(config_.output_file, csv_path);
        fmt::print("\n");

        if (result.success) {
            Common::Log::log_message(spdlog::level::info, kModule, fmt::format(
                                         "Conversion complete: {} frames, {} points",
                                         result.frames_converted, result.points_converted));
        } else {
            Common::Log::log_message(spdlog::level::warn, kModule,
                                     fmt::format("Conversion failed: {}", result.error_message));
        }
    }

    receiver_.reset();
    Common::Log::log_message(spdlog::level::info, kModule,
                             fmt::format("LiDAR [{}] shutdown complete", config_.lidar_position));
}


void LidarDriverApp::request_shutdown() {
    terminate_.store(true, std::memory_order_release);
}


std::atomic<bool> &LidarDriverApp::terminate_flag() {
    return terminate_;
}
