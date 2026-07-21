/// @brief Unified entry point for running the INS401, LMS4xxx, JAI Go-X and
/// Septentrio AsteRx drivers concurrently
///
/// Supported topology: one AsteRx receiver, one INS401, multiple LMS4xxx LiDARs
/// (one app per instance in the shared YAML) and multiple Go-X cameras (all
/// managed by a single GoxDriverApp via the cameras[] array in its JSON). Each
/// driver runs in its own thread; shutdown is orderly via shared terminate flags

#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <spdlog/fmt/chrono.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "activity_spinner.h"
#include "asterx_driver_app.h"
#include "data_type.h"
#include "driver_app.h"
#include "driver_markers.h"
#include "gox_driver_app.h"
#include "ins401_driver_app.h"
#include "lms4xxx_driver_app.h"
#include "lms4xxx_tool.h"
#include "signal_handler.h"
#include "utility.h"


namespace {
    constexpr std::string_view kModule = Common::Markers::kModuleMain;

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
                             std::string(Common::Markers::kStartingDrivers) + std::string(run_asterx ? " [AsteRx]" : "") +
                             std::string(run_gox ? " [GoX]" : "") + std::string(run_ins401 ? " [INS401]" : "") +
                             std::string(run_lms4xxx ? " [LMS4xxx]" : ""));

    // Create driver apps behind the unified IDriverApp interface. LMS4xxx maps
    // one app per LiDAR instance; multiple Go-X cameras are managed inside the
    // single GoxDriverApp (cameras[] in its JSON)
    struct DriverSlot {
        std::unique_ptr<Common::IDriverApp> app;
        std::string_view name;           // failure-log prefix ("AsteRx"...), GUI maps it to a sensor
        std::string_view init_failed;    // Common::Markers::k*InitFailed
        std::string_view run_exception;  // Common::Markers::k*RunException
        std::thread thread;
    };
    std::vector<DriverSlot> drivers;

    // Uniform bring-up per enabled driver: resolve the driver config path, snapshot it into <data_folder>/config/,
    // then create the app(s)
    const auto resolve_path = [&exe_dir](const std::string &relative) {
        return (exe_dir / "../../" / relative).string();
    };
    const auto copy_config = [&main_config](const std::string &src, std::string_view name, std::string_view ext) {
        try {
            std::filesystem::copy_file(src,
                                       fmt::format("{}/config/config-{}_{}.{}", main_config.data_folder_path, name,
                                                   main_config.timestamp, ext),
                                       std::filesystem::copy_options::overwrite_existing);
        } catch (const std::exception &e) {
            // Terminate program
            Common::Log::log_and_throw(kModule, fmt::format("Cannot copy {} config", name), e.what());
        }
    };

    if (run_asterx) {
        // Currently we only support one AsteRx equipment
        copy_config(resolve_path(main_config.asterx_config_path), "asterx", "yaml");
        drivers.push_back({ std::make_unique<AsterxDriverApp>(main_config), "AsteRx",
                            Common::Markers::kAsterxInitFailed, Common::Markers::kAsterxRunException, {} });
    }
    if (run_gox) {
        copy_config(resolve_path(main_config.gox_config_path), "gox", "json");
        drivers.push_back({ std::make_unique<GoxDriverApp>(main_config), "GoX",
                            Common::Markers::kGoxInitFailed, Common::Markers::kGoxRunException, {} });
    }
    if (run_ins401) {
        // Currently we only support one INS401 equipment
        copy_config(resolve_path(main_config.ins401_config_path), "ins401", "yaml");
        drivers.push_back({ std::make_unique<Ins401DriverApp>(main_config), "INS401",
                            Common::Markers::kIns401InitFailed, Common::Markers::kIns401RunException, {} });
    }
    if (run_lms4xxx) {
        const std::string lms4xxx_config_path = resolve_path(main_config.lms4xxx_config_path);
        copy_config(lms4xxx_config_path, "lms4xxx", "yaml");
        auto lms4xxx_configs = LMS4xxxTool::LoadConfigs(lms4xxx_config_path);
        for (auto &cfg: lms4xxx_configs) {
            cfg.data_folder_path = main_config.data_folder_path;
            cfg.timestamp = main_config.timestamp;
            drivers.push_back({ std::make_unique<Lms4xxxDriverApp>(std::move(cfg)), "LMS4xxx",
                                Common::Markers::kLms4xxxInitFailed, Common::Markers::kLms4xxxRunException, {} });
        }
    }

    // Initialize drivers in creation order. The predicate keeps Ctrl+C
    // responsive during blocking bring-ups (AsteRx: TCP connect + command
    // sequence; GoX: camera discovery retries + PTP convergence can take tens
    // of seconds). On an interrupt, remaining drivers are skipped and the rig
    // proceeds straight to the orderly shutdown of what already runs.
    const auto external_stop = [] { return g_terminate.load(std::memory_order_acquire); };
    std::size_t initialized = 0;
    for (auto &d: drivers) {
        if (!d.app->init(external_stop)) {
            if (g_terminate.load(std::memory_order_acquire)) {
                Common::Log::log_message(spdlog::level::warn, kModule,
                                         fmt::format("{} bring-up interrupted, shutting down", d.name));
                break;
            }
            // Program will be terminated
            Common::Log::log_and_throw(kModule, d.init_failed);
        }
        ++initialized;
    }

    // Run the successfully initialized drivers concurrently, one thread each.
    // No push_back after this point: the lambdas hold references into `drivers`
    for (std::size_t i = 0; i < initialized; ++i) {
        auto &d = drivers[i];
        d.thread = std::thread([&d]() {
            try {
                d.app->run();
            } catch (const std::exception &e) {
                // Never rethrow inside a std::thread (std::terminate); log and
                // let the terminate flag propagate an orderly shutdown
                Common::Log::log_and_throw(kModule, d.run_exception, e.what(), /*throw_error=*/false);
                d.app->TerminateFlag().store(true, std::memory_order_release);
            }
        });
    }

    // Activity spinner: shows animation during idle periods (1.5s no console output)
    Common::ActivitySpinner spinner((exe_dir / "../../resource/spinner_frames.conf").string());
    spinner.Attach();

    // Any driver's termination (or a signal) takes the whole rig down together
    while (!g_terminate.load(std::memory_order_acquire)) {
        const bool any_terminated =
                std::any_of(drivers.begin(), drivers.end(),
                            [](DriverSlot &d) { return d.app->TerminateFlag().load(std::memory_order_acquire); });
        if (any_terminated) {
            break;
        }
        spinner.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spinner.Detach();
    spinner.Clear();

    // Propagate termination to all drivers
    for (auto &d: drivers) {
        d.app->TerminateFlag().store(true, std::memory_order_release);
    }

    if (int sig = g_signal_received.load(std::memory_order_relaxed); sig != 0) {
        Common::Log::log_message(spdlog::level::warn, kModule,
                                 fmt::format(fmt::runtime(Common::Markers::kReceivedSignalTpl), sig));
    }

    // Join run threads, then shut down in reverse creation order (shutdown()
    // is idempotent and safe after a failed or interrupted init)
    for (auto &d: drivers) {
        if (d.thread.joinable()) {
            d.thread.join();
        }
    }
    for (auto it = drivers.rbegin(); it != drivers.rend(); ++it) {
        it->app->shutdown();
    }

    Common::Log::log_message(spdlog::level::info, kModule, Common::Markers::kAllDriversShutDown);
    return 0;
}
