/// @file src/main.cpp
/// @brief Unified entry point for running INS401 and LMS41xxx drivers concurrently.
///
/// Installs a single signal handler with a shared terminate flag, then propagates
/// it to both driver apps. Each driver runs in its own thread; shutdown is orderly.

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/chrono.h>

#include "utility.h"
#include "data_type.h"
#include "signal_handler.h"
#include "ins401_driver_app.h"
#include "lidar_driver_app.h"


namespace {
    constexpr std::string_view kModule = "Main";

    void LoadConfig(std::string_view config_path, Common::Config &config) {
        // Delegate YAML I/O to the common ConfigLoader (throws on error).
        Common::ConfigLoader loader(config_path);
        const auto &root = loader.root();

        // --- General ---
        const auto &general = root["General"];
        config.output_directory = general["Output Directory"].as<std::string>("./data");
        config.run_mode = general["Run Mode"].as<int>(0);
        config.ins_config_path = general["INS401 Driver Config Path"].as<std::string>(
            "./ins401_driver/config/config-ins401.yaml");
        config.lidar_config_path = general["LMS4XXX Driver Config Path"].as<std::string>(
            "./lms4xxx_driver/config/config-lms4xxx.yaml");
        config.lidar_launch_path = general["LMS4XXX Driver Launch Path"].as<std::string>(
            "./lms4xxx_driver/config/sick_lms_4xxx.launch");

        // --- Logging System ---
        const auto &logging = root["Logging System"];
        config.enable_logging = logging["Enable Logging"].as<bool>(true);
    }


    void InitializeSystem(Common::Config &config) {
        // Prepare data directory
        const auto now = std::chrono::system_clock::now();
        config.timestamp = fmt::format("{:%Y%m%d_%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(now));
        config.data_folder_path = fmt::format("{}/{}", config.output_directory, config.timestamp);
        std::filesystem::create_directories(config.data_folder_path);
        // Init logging system
        if (config.enable_logging) {
            const std::string log_file = fmt::format("{}/log_drivers_{}.log", config.data_folder_path, config.timestamp);
            Common::Logger::init({log_file, false}, "AmigaDrivers");
        }
    }
} // namespace


int main(int argc, char *argv[]) {
    // Install a single signal handler with a shared terminate flag
    static std::atomic<bool> g_terminate{false};
    static std::atomic<int> g_signal_received{0};
    Common::SignalHandler::install(g_terminate, g_signal_received, {SIGINT, SIGTERM, SIGABRT, SIGTSTP, SIGHUP});


    // Read main config and initialize system (e.g. create timestamped data folder)
    std::string main_config_path = argc > 1 ? argv[1] : "../../config/config-main.yaml";
    Common::Config main_config;
    LoadConfig(main_config_path, main_config);
    InitializeSystem(main_config);


    // Determine which drivers to run
    bool run_ins = false;
    bool run_lidar = false;
    switch (main_config.run_mode) {
        case 0:
            // Default: run both
            run_lidar = true;
            run_ins = true;
            break;
        case 1:
            // Run only INS401
            run_ins = true;
            break;
        case 2:
            // Run only LMS4xxx
            run_lidar = true;
            break;
        default:
            Common::Log::log_message(spdlog::level::warn, kModule,
                                     fmt::format("Invalid Run Mode {}, defaulting to both drivers",
                                                 main_config.run_mode));
            run_ins = true;
            run_lidar = true;
    }
    Common::Log::log_message(spdlog::level::info, kModule,
                             "Starting Amiga Drivers" + std::string(run_ins ? " [INS401]" : "") + std::string(
                                 run_lidar ? " [LMS4xxx]" : ""));


    // Create driver apps
    std::unique_ptr<InsDriverApp> ins_app;
    std::unique_ptr<LidarDriverApp> lidar_app;

    if (run_ins) {
        ins_app = std::make_unique<InsDriverApp>(main_config);
    }
    if (run_lidar) {
        lidar_app = std::make_unique<LidarDriverApp>(main_config);
    }

    // Initialize drivers
    if (ins_app) {
        try {
            if (!ins_app->init()) {
                Common::Log::log_message(spdlog::level::warn, kModule, "INS401 driver initialization failed");
                ins_app.reset();
                run_ins = false;
            }
        } catch (const std::exception &e) {
            // Use spdlog::error() directly — log_message(err) would re-throw.
            spdlog::error("[Main] INS401 init exception: {}", e.what());
            ins_app.reset();
            run_ins = false;
        }
    }
    if (lidar_app) {
        try {
            if (!lidar_app->init()) {
                Common::Log::log_message(spdlog::level::warn, kModule, "LMS4xxx driver initialization failed");
                lidar_app.reset();
                run_lidar = false;
            }
        } catch (const std::exception &e) {
            // Use spdlog::error() directly — log_message(err) would re-throw.
            spdlog::error("[Main] LMS4xxx init exception: {}", e.what());
            lidar_app.reset();
            run_lidar = false;
        }
    }

    if (!ins_app && !lidar_app) {
        Common::Log::log_message(spdlog::level::warn, kModule, "No drivers initialized, exiting");
        return 1;
    }


    // Run both drivers concurrently
    std::thread ins_thread;
    std::thread lidar_thread;
    if (ins_app) {
        ins_thread = std::thread([&ins_app]() {
            try {
                ins_app->run();
            } catch (const std::exception &e) {
                spdlog::error("[Main] INS401 run() exception: {}", e.what());
                ins_app->terminate_flag().store(true, std::memory_order_release);
            }
        });
    }
    if (lidar_app) {
        lidar_thread = std::thread([&lidar_app]() {
            try {
                lidar_app->run();
            } catch (const std::exception &e) {
                spdlog::error("[Main] LMS4xxx run() exception: {}", e.what());
                lidar_app->terminate_flag().store(true, std::memory_order_release);
            }
        });
    }


    // Monitor shared terminate flag and propagate to drivers
    while (!g_terminate.load(std::memory_order_acquire)) {
        // Also check if either driver self-terminated (e.g. receiver exception).
        if (ins_app && ins_app->terminate_flag().load(std::memory_order_acquire)) {
            break;
        }
        if (lidar_app && lidar_app->terminate_flag().load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }


    // Propagate termination to both drivers.
    if (ins_app) {
        ins_app->terminate_flag().store(true, std::memory_order_release);
    }
    if (lidar_app) {
        lidar_app->terminate_flag().store(true, std::memory_order_release);
    }

    if (int sig = g_signal_received.load(std::memory_order_relaxed); sig != 0) {
        Common::Log::log_message(spdlog::level::warn, kModule,
                                 fmt::format("Received signal {}, shutting down...", sig));
    }


    // Join driver threads
    if (ins_thread.joinable()) {
        ins_thread.join();
    }
    if (lidar_thread.joinable()) {
        lidar_thread.join();
    }


    // Shutdown both drivers
    Common::Log::log_message(spdlog::level::info, kModule, "Shutting down drivers...");

    if (ins_app) {
        ins_app->shutdown();
    }
    if (lidar_app) {
        lidar_app->shutdown();
    }


    // Note: LiDAR post-recording CSV conversion (if enabled) is handled
    // inside LidarDriverApp::shutdown(), matching the INS401 pattern where
    // ProcessBinaryFiles() runs during the receiver shutdown sequence.

    Common::Log::log_message(spdlog::level::info, kModule, "All drivers shut down");
    return 0;
}
