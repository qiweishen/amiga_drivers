/// @file lms4xxx_driver/src/main.cpp
/// @brief Standalone entry point for the SICK LMS4XXX LiDAR driver.
///
/// Supports CLI arguments for configuration, standalone CSV conversion mode,
/// and normal driver mode with optional post-recording conversion.

#include <spdlog/spdlog.h>
#include <cinttypes>
#include <iostream>
#include <string>

#include "csv_converter.h"
#include "lidar_driver_app.h"
#include "lidar_tool.h"
#include "utility.h"
#include "signal_handler.h"


static void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [options]\n"
            << "  --config <path>    YAML config file (default: ../config/config-lms4xxx.yaml)\n"
            << "  --launch <path>    SICK launch file (overrides config)\n"
            << "  --output <path>    Output binary file (overrides config)\n"
            << "  --ntp-server <ip>  NTP server IP (overrides config)\n"
            << "  --csv              Convert output to CSV after recording\n"
            << "  --convert <path>   Convert existing binary file to CSV and exit\n"
            << "  --quiet, -q        Suppress SICK library console output\n"
            << "  --log <path>       Log file path\n"
            << "  --help, -h         Show this help\n";
}


int main(int argc, char *argv[]) {
    LiDARConfig config;
    bool standalone_convert = false;
    std::string convert_input;
    std::string config_path = "../config/config-lms4xxx.yaml";

    // Parse command-line arguments.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--launch" && i + 1 < argc) {
            config.launch_file = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            config.output_file = argv[++i];
        } else if (arg == "--ntp-server" && i + 1 < argc) {
            config.ntp_server_ip = argv[++i];
        } else if (arg == "--csv") {
            config.convert_to_csv = true;
        } else if (arg == "--convert" && i + 1 < argc) {
            standalone_convert = true;
            convert_input = argv[++i];
        } else if (arg == "--quiet" || arg == "-q") {
            config.quiet = true;
        } else if (arg == "--log" && i + 1 < argc) {
            config.log_file = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            // Legacy positional argument support.
            if (i == 1) {
                config.launch_file = arg;
            } else if (i == 2) {
                config.output_file = arg;
            }
        }
    }

    // Load YAML config, then apply CLI overrides on top.
    {
        LiDARConfig yaml_config;
        try {
            LidarTool::LoadConfig(config_path, yaml_config);
        } catch (const std::exception &) {
            // Config file is optional in standalone mode.
        }

        // Apply YAML values only for fields not set via CLI.
        if (config.launch_file.empty()) {
            config.launch_file = yaml_config.launch_file;
        }
        if (config.output_file.empty()) {
            config.output_file = yaml_config.output_file;
        }
        if (config.ntp_server_ip.empty()) {
            config.ntp_server_ip = yaml_config.ntp_server_ip;
        }
    }

    // Default output file if still empty.
    if (config.output_file.empty()) {
        config.output_file = "./data/pointcloud.bin";
    }

    // Initialize logging before any work.
    Common::Logger::init({config.log_file, config.quiet}, "LMS4xxx Driver");

    // Standalone CSV conversion mode.
    if (standalone_convert) {
        std::string csv_path = BinaryToCsvConverter::default_csv_path(convert_input);
        Common::Log::log_message(spdlog::level::info, "Main",
                                 fmt::format("Converting {} -> {}", convert_input, csv_path));

        BinaryToCsvConverter converter;
        converter.set_progress_callback([](uint64_t frames, uint64_t bytes, uint64_t total) {
            double pct = total > 0 ? (100.0 * static_cast<double>(bytes) / static_cast<double>(total)) : 0.0;
            std::cout << "\r[Converter] Frames: " << frames
                    << "  Progress: " << static_cast<int>(pct) << "%" << std::flush;
        });

        auto result = converter.convert(convert_input, csv_path);
        std::cout << std::endl;

        if (result.success) {
            Common::Log::log_message(spdlog::level::info, "Main", fmt::format(
                                         "Conversion complete: {} frames, {} points",
                                         result.frames_converted, result.points_converted));
            return 0;
        } else {
            Common::Log::log_message(spdlog::level::warn, "Main",
                                     fmt::format("Conversion failed: {}", result.error_message));
            return 1;
        }
    }

    // Normal driver mode. CSV conversion (if --csv) happens inside shutdown().
    LidarDriverApp app(std::move(config));

    Common::SignalHandler::install(app.terminate_flag());

    if (!app.init()) {
        return 1;
    }

    app.run();

    Common::Log::log_message(spdlog::level::info, "Main", "Shutting down...");
    app.shutdown();

    return 0;
}
