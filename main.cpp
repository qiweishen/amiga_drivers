/// @brief Unified entry point for running the INS401, LMS4xxx, JAI Go-X and
/// Septentrio AsteRx drivers concurrently
///
/// Supported topology: one AsteRx receiver, one INS401, multiple LMS4xxx LiDARs
/// (one app per instance in the shared YAML) and multiple Go-X cameras (all
/// managed by a single GoxDriverApp via the cameras[] array in its JSON). Each
/// driver runs in its own thread; shutdown is orderly via shared terminate flags

#include <atomic>
#include <csignal>
#include <iostream>
#include <spdlog/fmt/chrono.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "activity_spinner.h"
#include "asterx_driver_app.h"
#include "data_type.h"
#include "gox_driver_app.h"
#include "ins401_driver_app.h"
#include "lms4xxx_driver_app.h"
#include "lms4xxx_tool.h"
#include "signal_handler.h"
#include "utility.h"


namespace {
    constexpr std::string_view kModule = "Main";

    void LoadConfig(std::string_view config_path, Common::Config &config) {
        // Delegate YAML I/O to the common ConfigLoader (throws on error)
        Common::ConfigLoader loader(config_path);
        const auto &root = loader.root();

        // General
        const auto &general = root["General"];
        config.output_directory = general["Output Directory"].as<std::string>("./data");
        config.enable_ins401 = general["Enable INS401"].as<bool>(true);
        config.enable_lms4xxx = general["Enable LMS4XXX"].as<bool>(true);
        config.enable_gox = general["Enable GOX"].as<bool>(false);
        config.enable_asterx = general["Enable ASTERX"].as<bool>(false);
        config.ins401_config_path = general["INS401 Driver Config Path"].as<std::string>(
            "./ins401_driver/config/config-ins401.yaml");
        config.lms4xxx_config_path =
                general["LMS4XXX Driver Config Path"].as<std::string>("./lms4xxx_driver/config/config-lms4xxx.yaml");
        config.gox_config_path = general["GOX Driver Config Path"].as<std::string>(
            "./gox_driver/config/config-gox.json");
        config.asterx_config_path =
                general["ASTERX Driver Config Path"].as<std::string>("./asterx_driver/config/config-asterx.yaml");

        // Logging System
        const auto &logging = root["Logging System"];
        config.enable_logging = logging["Enable Logging"].as<bool>(true);
    }


    void InitializeSystem(Common::Config &config) {
        // Prepare data directory
        const auto now = std::chrono::system_clock::now();
        config.timestamp = fmt::format("{:%Y%m%d_%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(now));
        config.data_folder_path = fmt::format("{}/{}", config.output_directory, config.timestamp);
        std::filesystem::create_directories(config.data_folder_path);
        std::filesystem::create_directories(fmt::format("{}/{}", config.data_folder_path, "config"));

        // Init logging system
        if (config.enable_logging) {
            const std::string log_file = fmt::format("{}/log_{}.log", config.data_folder_path, config.timestamp);
            Common::Logger::init({log_file, false}, "AmigaDrivers");
        }
    }
} // namespace


int main(int argc, char *argv[]) {
    // Install a single signal handler with a shared terminate flag
    static std::atomic<bool> g_terminate{false};
    static std::atomic<int> g_signal_received{0};
    Common::SignalHandler::install(g_terminate, g_signal_received, {SIGINT, SIGTERM, SIGABRT, SIGTSTP, SIGHUP});
    // A peer reset on a socket write (NTRIP TLS, GVSP) must surface as an error return, not kill the process via SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);

    // Read main config and initialize system (e.g. create timestamped data folder)
    std::filesystem::path exe_dir = Common::GetExecutableDir(); // exe_dir + "../../" -> project root
    std::string main_config_path = (exe_dir / "../../config/config-main.yaml");
    main_config_path = argc > 1 ? argv[1] : main_config_path;
    Common::Config main_config;
    LoadConfig(main_config_path, main_config);
    InitializeSystem(main_config);

    // Determine which drivers to run (per-driver enable switches)
    const bool run_asterx = main_config.enable_asterx;
    const bool run_gox = main_config.enable_gox;
    const bool run_ins401 = main_config.enable_ins401;
    const bool run_lms4xxx = main_config.enable_lms4xxx;
    if (!run_asterx && !run_gox && !run_ins401 && !run_lms4xxx) {
        Common::Log::log_and_throw(kModule, "No drivers enabled in the main config");
    }
    Common::Log::log_message(spdlog::level::info, kModule,
                             "Starting Amiga Drivers" + std::string(run_asterx ? " [AsteRx]" : "") +
                             std::string(run_gox ? " [GoX]" : "") + std::string(run_ins401 ? " [INS401]" : "") +
                             std::string(run_lms4xxx ? " [LMS4xxx]" : ""));

    // Create driver apps. LMS4xxx maps one app per LiDAR instance; multiple Go-X
    // cameras are managed inside the single GoxDriverApp (cameras[] in its JSON)
    std::unique_ptr<AsterxDriverApp> asterx_app;
    std::unique_ptr<GoxDriverApp> gox_app;
    std::unique_ptr<Ins401DriverApp> ins401_app;
    std::vector<std::unique_ptr<Lms4xxxDriverApp> > lms4xxx_apps;

    // Uniform bring-up per enabled driver: resolve the driver config path, snapshot
    // it into <data_folder>/config/, then create the app(s)
    const auto resolve_path = [&exe_dir](const std::string &relative) { return (exe_dir / "../../" / relative).string(); };
    const auto copy_config = [&main_config](const std::string &src, std::string_view name, std::string_view ext) {
        try {
            std::filesystem::copy_file(src,
                                       fmt::format("{}/config/config-{}_{}.{}", main_config.data_folder_path, name,
                                                   main_config.timestamp, ext),
                                       std::filesystem::copy_options::overwrite_existing);
        } catch (const std::exception &e) {
            Common::Log::log_and_throw(kModule, fmt::format("Cannot copy {} config", name), e.what(), true);
        }
    };

    if (run_asterx) {
        // Currently we only support one AsteRx equipment
        copy_config(resolve_path(main_config.asterx_config_path), "asterx", "yaml");
        asterx_app = std::make_unique<AsterxDriverApp>(main_config);
    }
    if (run_gox) {
        copy_config(resolve_path(main_config.gox_config_path), "gox", "json");
        gox_app = std::make_unique<GoxDriverApp>(main_config);
    }
    if (run_ins401) {
        // Currently we only support one INS401 equipment
        copy_config(resolve_path(main_config.ins401_config_path), "ins401", "yaml");
        ins401_app = std::make_unique<Ins401DriverApp>(main_config);
    }
    if (run_lms4xxx) {
        const std::string lms4xxx_config_path = resolve_path(main_config.lms4xxx_config_path);
        copy_config(lms4xxx_config_path, "lms4xxx", "yaml");
        auto lms4xxx_configs = LMS4xxxTool::LoadConfigs(lms4xxx_config_path);
        for (auto &cfg: lms4xxx_configs) {
            cfg.data_folder_path = main_config.data_folder_path;
            cfg.timestamp = main_config.timestamp;
            lms4xxx_apps.push_back(std::make_unique<Lms4xxxDriverApp>(std::move(cfg)));
        }
        Common::Log::log_message(spdlog::level::info, kModule,
                                 fmt::format("Created {} LiDAR instance(s)", lms4xxx_apps.size()));
    }

    // Initialize drivers
    if (ins401_app) {
        if (!ins401_app->init()) {
            // Program will be terminated
            Common::Log::log_and_throw(kModule, "INS401 driver initialization failed");
        }
    }
    for (auto it = lms4xxx_apps.begin(); it != lms4xxx_apps.end();) {
        if (!(*it)->init()) {
            // Program will be terminated
            Common::Log::log_and_throw(kModule, "LMS4xxx driver initialization failed");
        }
        ++it;
    }

    // AsteRx bring-up is bounded (TCP connect + command sequence; failures are fail-fast before the first configure);
    // it blocks until the receiver is configured and recording. The predicate keeps Ctrl+C responsive.
    if (asterx_app) {
        if (!asterx_app->init([] { return g_terminate.load(std::memory_order_acquire); })) {
            if (g_terminate.load(std::memory_order_acquire)) {
                // Interrupted by a signal during bring-up: fall through to the orderly shutdown of the already-running drivers
                Common::Log::log_message(spdlog::level::warn, kModule, "AsteRx bring-up interrupted, shutting down");
            } else {
                // Program will be terminated
                Common::Log::log_and_throw(kModule, "AsteRx driver initialization failed");
            }
        }
    }

    // Last: camera discovery retries + PTP convergence can take tens of seconds and must not delay the cheaper
    // drivers' init. The predicate keeps Ctrl+C responsive during the blocking bring-up.
    if (gox_app) {
        if (!gox_app->init([] { return g_terminate.load(std::memory_order_acquire); })) {
            if (g_terminate.load(std::memory_order_acquire)) {
                // Interrupted by a signal during bring-up: fall through to the orderly shutdown of the already-running drivers
                Common::Log::log_message(spdlog::level::warn, kModule, "GoX bring-up interrupted, shutting down");
            } else {
                // Program will be terminated
                Common::Log::log_and_throw(kModule, "GoX driver initialization failed");
            }
        }
    }

    // Run all drivers concurrently
    std::thread ins_thread;
    std::vector<std::thread> lidar_threads;
    if (ins401_app) {
        ins_thread = std::thread([&ins401_app]() {
            try {
                ins401_app->run();
            } catch (const std::exception &e) {
                // Program will be terminated
                Common::Log::log_and_throw(kModule, "INS401 run() exception", e.what());
            }
        });
    }
    for (auto &app: lms4xxx_apps) {
        lidar_threads.emplace_back([&app]() {
            try {
                app->run();
            } catch (const std::exception &e) {
                Common::Log::log_and_throw(kModule, "LMS4xxx run() exception", e.what());
            }
        });
    }
    std::thread gox_thread;
    if (gox_app) {
        gox_thread = std::thread([&gox_app]() {
            try {
                gox_app->run();
            } catch (const std::exception &e) {
                Common::Log::log_and_throw(kModule, "GoX run() exception", e.what());
            }
        });
    }
    std::thread asterx_thread;
    if (asterx_app) {
        asterx_thread = std::thread([&asterx_app]() {
            try {
                asterx_app->run();
            } catch (const std::exception &e) {
                Common::Log::log_and_throw(kModule, "AsteRx run() exception", e.what());
            }
        });
    }

    // Activity spinner: shows animation during idle periods (1.5s no console output)
    Common::ActivitySpinner spinner((exe_dir / "../../resource/spinner_frames.conf").string());
    spinner.Attach();

    while (!g_terminate.load(std::memory_order_acquire)) {
        if (ins401_app && ins401_app->TerminateFlag().load(std::memory_order_acquire)) {
            break;
        }
        bool any_lidar_terminated = false;
        for (auto &app: lms4xxx_apps) {
            if (app->TerminateFlag().load(std::memory_order_acquire)) {
                any_lidar_terminated = true;
                break;
            }
        }
        if (any_lidar_terminated)
            break;
        if (gox_app && gox_app->TerminateFlag().load(std::memory_order_acquire)) {
            break;
        }
        if (asterx_app && asterx_app->TerminateFlag().load(std::memory_order_acquire)) {
            break;
        }
        spinner.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spinner.Detach();
    spinner.Clear();

    // Propagate termination to all drivers
    if (ins401_app) {
        ins401_app->TerminateFlag().store(true, std::memory_order_release);
    }
    for (auto &app: lms4xxx_apps) {
        app->TerminateFlag().store(true, std::memory_order_release);
    }
    if (gox_app) {
        gox_app->TerminateFlag().store(true, std::memory_order_release);
    }
    if (asterx_app) {
        asterx_app->TerminateFlag().store(true, std::memory_order_release);
    }

    if (int sig = g_signal_received.load(std::memory_order_relaxed); sig != 0) {
        Common::Log::log_message(spdlog::level::warn, kModule,
                                 fmt::format("Received signal {}, shutting down all drivers...", sig));
    }

    // Join driver threads
    if (ins_thread.joinable()) {
        ins_thread.join();
    }
    for (auto &t: lidar_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    if (gox_thread.joinable()) {
        gox_thread.join();
    }
    if (asterx_thread.joinable()) {
        asterx_thread.join();
    }

    // Shutdown all drivers
    if (ins401_app) {
        ins401_app->shutdown();
    }
    for (auto &app: lms4xxx_apps) {
        app->shutdown();
    }
    if (gox_app) {
        gox_app->shutdown();
    }
    if (asterx_app) {
        asterx_app->shutdown();
    }

    Common::Log::log_message(spdlog::level::info, kModule, "All drivers shut down");
    return 0;
}
