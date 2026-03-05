#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <spdlog/spdlog.h>

#include "signal_handler.h"
#include "utility.h"
#include "lms4xxx_callbacks.h"
#include "lms4xxx_config.h"
#include "lms4xxx_driver.h"
#include "lms4xxx_scan_data.h"


namespace {
    constexpr std::string_view kModule = "BasicScan";

    std::atomic<std::uint64_t> g_frame_count{0};
}  // namespace


int main() {
    // Signal handling
    static std::atomic<bool> g_terminate{false};
    Common::SignalHandler::install(g_terminate, {SIGINT, SIGTERM});

    // Logging
    Common::Logger::init({"", false}, "LMS4xxxExample");

    // Configuration (default settings)
    LMS4xxx::DriverConfig config;
    config.device.ip = "192.168.0.1";
    config.device.port = 2111;
    config.scan.enable_distance = true;
    config.scan.enable_rssi = false;
    config.scan.start_angle_deg = 55.0;
    config.scan.stop_angle_deg = 125.0;
    config.scan.angular_resolution_deg = 0.0833;

    // Create driver
    LMS4xxx::LMS4xxxDriver driver(config);

    // Register callbacks
    driver.SetScanCallback([](const LMS4xxx::ScanData &scan) {
        g_frame_count.fetch_add(1, std::memory_order_relaxed);
    });

    driver.SetConnectionCallback([](LMS4xxx::ConnectionState state) {
        Common::Log::log_message(spdlog::level::info, kModule,
            std::string("Connection state: ") + LMS4xxx::ToString(state));
    });

    driver.SetErrorCallback([](std::error_code ec, const std::string &detail) {
        Common::Log::log_message(spdlog::level::err, kModule,
            ec.message() + (detail.empty() ? "" : ": " + detail));
    });

    // Connect
    Common::Log::log_message(spdlog::level::info, kModule,
        "Connecting to " + config.device.ip + ":" + std::to_string(config.device.port));

    if (auto ec = driver.Connect(); ec) {
        Common::Log::log_message(spdlog::level::err, kModule,
            "Connect failed: " + ec.message());
        return 1;
    }

    // Configure
    if (auto ec = driver.Configure(); ec) {
        Common::Log::log_message(spdlog::level::err, kModule,
            "Configure failed: " + ec.message());
        driver.Disconnect();
        return 1;
    }

    // Start scanning
    if (auto ec = driver.StartScanning(); ec) {
        Common::Log::log_message(spdlog::level::err, kModule,
            "Start scanning failed: " + ec.message());
        driver.Disconnect();
        return 1;
    }

    Common::Log::log_message(spdlog::level::info, kModule, "Scanning started. Press Ctrl+C to stop.");

    // Main loop: log statistics periodically
    auto last_log = std::chrono::steady_clock::now();
    while (!g_terminate.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(5)) {
            driver.LogStatistics();
            auto frames = g_frame_count.load(std::memory_order_relaxed);
            Common::Log::log_message(spdlog::level::info, kModule,
                "Total frames received: " + std::to_string(frames));
            last_log = now;
        }
    }

    // Shutdown
    Common::Log::log_message(spdlog::level::info, kModule, "Shutting down...");
    driver.StopScanning();
    driver.LogStatistics();
    driver.Disconnect();

    Common::Log::log_message(spdlog::level::info, kModule, "Done.");
    return 0;
}
