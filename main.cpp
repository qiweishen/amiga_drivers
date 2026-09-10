#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <spdlog/fmt/chrono.h>
#include <spdlog/spdlog.h>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "asterx_driver_app.h"
#include "data_type.h"
#include "driver_app.h"
#include "driver_markers.h"
#include "drivers_json.h"
#include "fx10_driver_app.h"
#include "gox_driver_app.h"
#include "lms4xxx_driver_app.h"
#include "signal_handler.h"
#include "utility.h"
#include "logger.h"
#include "run_guards.h"


// Baked by the root CMakeLists (target_compile_definitions on AmigaDrivers)
#ifndef AMIGA_DRIVERS_VERSION
#define AMIGA_DRIVERS_VERSION "unknown"
#endif
#ifndef AMIGA_DRIVERS_GIT_SHA
#define AMIGA_DRIVERS_GIT_SHA "unknown"
#endif


namespace {
    constexpr std::string_view kModule = common::Markers::kModuleMain;

    void LoadConfig(const std::filesystem::path &project_root, const std::filesystem::path &config_path, common::Config &config) {
        // Delegate YAML I/O to the common ConfigLoader (throws on error)
        common::ConfigLoader loader(config_path.string());
        const auto &root = loader.Root();

        // General
        const auto &general = root["General"];
        config.operator_name = general["Operator"].as<std::string>("Anonymous");
        config.field_name = general["Field"].as<std::string>("Unknown");

        config.output_directory = general["Output Directory"].as<std::string>("./data");

        config.enable_lms4xxx = general["Enable LMS4XXX"].as<bool>(true);
        config.enable_gox = general["Enable GOX"].as<bool>(false);
        config.enable_asterx = general["Enable ASTERX"].as<bool>(false);
        config.enable_fx10 = general["Enable FX10"].as<bool>(false);

        config.asterx_config_path = general["ASTERX Driver Config Path"].as<std::string>(
            "./asterx_driver/config/config-asterx.yaml");
        config.fx10_config_path = general["FX10 Driver Config Path"].as<std::string>(
            "./fx10_driver/config/config-fx10.yaml");
        config.gox_config_path = general["GOX Driver Config Path"].as<std::string>(
            "./gox_driver/config/config-gox.yaml");
        config.lms4xxx_config_path = general["LMS4XXX Driver Config Path"].as<std::string>(
            "./lms4xxx_driver/config/config-lms4xxx.yaml");

        // Guards: one disk floor and one no-data watchdog for the whole rig.
        // Unlike everything above, these are range-checked — a guard that is
        // silently misconfigured is worse than no guard, because the log still
        // says it is armed.
        // By value, and every access short-circuits on the block itself:
        // yaml-cpp THROWS when a missing node is indexed, so a config written
        // before this block existed must fall back to the defaults, not abort.
        const YAML::Node guards = root["Guards"];
        const auto read_guard = [&guards, &config_path](std::string_view key, double fallback) {
            const std::string name{key};
            const double value = guards && guards[name] ? guards[name].as<double>(fallback) : fallback;
            if (!(value >= 0.0) || value > 86400.0) {
                common::Log::LogAndThrow(kModule,
                                           fmt::format("Guards: {} must be between 0 (off) and 86400 in '{}'", key,
                                                       config_path.string()));
            }
            return value;
        };
        config.guards.disk_min_free_gib = read_guard("Disk Min Free GiB", 5.0);
        config.guards.disk_warn_free_gib = read_guard("Disk Warn Free GiB", 20.0);
        config.guards.no_data_warn_s = read_guard("No Data Warn S", 5.0);
        config.guards.no_data_abort_s = read_guard("No Data Abort S", 60.0);
        // A warning floor at or below the hard floor never fires (the hard floor
        // stops the run first), so it is a typo, not a preference.
        if (config.guards.disk_warn_free_gib > 0.0 &&
            config.guards.disk_warn_free_gib <= config.guards.disk_min_free_gib) {
            common::Log::LogAndThrow(kModule,
                                       "Guards: 'Disk Warn Free GiB' must be greater than 'Disk Min Free GiB' "
                                       "(or 0 to disable the warning)");
        }
        if (config.guards.no_data_abort_s > 0.0 && config.guards.no_data_warn_s >= config.guards.no_data_abort_s) {
            common::Log::LogAndThrow(kModule,
                                       "Guards: 'No Data Warn S' must be smaller than 'No Data Abort S' "
                                       "(or 0 to disable the warning)");
        }

        const auto resolve_path = [&project_root](std::filesystem::path &input_path) {
            if (input_path.is_relative()) {
                input_path = project_root / input_path;
            }

            input_path = common::GetAbsolutePath(input_path);
        };

        resolve_path(config.output_directory);
        resolve_path(config.asterx_config_path);
        resolve_path(config.fx10_config_path);
        resolve_path(config.gox_config_path);
        resolve_path(config.lms4xxx_config_path);
    }


    void InitializeSystem(common::Config &config) {
        // Prepare data directory
        const auto now = std::chrono::system_clock::now();
        config.timestamp = fmt::format("{:%Y%m%d_%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(now));
        std::filesystem::create_directories(config.output_directory);
        const auto session_dir = config.output_directory / config.timestamp;
        // Claim the session atomically before any log/config/driver can write.
        // Keep the timestamp naming contract used by the GUI, but never reuse it.
        if (!std::filesystem::create_directory(session_dir)) {
            throw std::runtime_error("Session already exists; refusing to overwrite: " + session_dir.string());
        }
        config.data_folder_path = session_dir / "raw";
        std::filesystem::create_directories(config.data_folder_path);
        std::filesystem::create_directories(config.data_folder_path / "config");

        // Init logging system
        const std::string log_file = config.data_folder_path / fmt::format("log_{}.log", config.timestamp);
        common::Logger::Init({log_file, false}, "AmigaDrivers");
    }
} // namespace


int main(int argc, char *argv[]) {
    // Install a single signal handler with a shared terminate flag
    static std::atomic<bool> g_terminate{false};
    static std::atomic<int> g_signal_received{0};
    common::SignalHandler::Install(g_terminate, g_signal_received, {SIGINT, SIGTERM, SIGHUP});
    // A peer reset on a socket write (NTRIP TLS, GVSP) must surface as an error return, not kill the process via SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);

    // Read main config and initialize system (e.g., create timestamped data folder)
    std::filesystem::path project_root = common::GetAbsolutePath(common::GetExecutableDir() / "../../"); // exe_dir + "../../" -> project root
    std::filesystem::path main_config_path;
    if (argc > 1) {
        main_config_path = argv[1];
        if (main_config_path.is_relative()) {
            main_config_path = common::GetAbsolutePath(project_root / main_config_path);
        }
    } else {
        main_config_path = common::GetAbsolutePath(project_root / "config/config-main.yaml");
    }
    common::Config main_config;
    try {
        LoadConfig(project_root, main_config_path, main_config);
        InitializeSystem(main_config);
    } catch (const std::exception &e) {
        std::cerr << "Cannot initialize acquisition session: " << e.what() << '\n';
        return 1;
    }

    // Run-level summary at the data folder root; finalized on every exit path below
    common::DriversJson drivers_json(main_config.data_folder_path / "drivers.json");
    drivers_json.SetMeta(main_config.operator_name, main_config.field_name);
    drivers_json.SetRun(main_config.timestamp, main_config.output_directory);
    drivers_json.SetVersion(AMIGA_DRIVERS_VERSION, AMIGA_DRIVERS_GIT_SHA);

    // Determine which drivers to Run (per-driver enable switches)
    const bool run_asterx = main_config.enable_asterx;
    const bool run_fx10 = main_config.enable_fx10;
    const bool run_gox = main_config.enable_gox;
    const bool run_lms4xxx = main_config.enable_lms4xxx;

    // A configuration that cannot even be snapshotted ends the run before any
    // driver is created: a logged failure and exit code 1, not an uncaught
    // exception (abort, 134) that the GUI can only report as "crashed"
    const auto config_failed = [&drivers_json]() {
        drivers_json.Finalize("failed (configuration)");
        return 1;
    };
    if (!run_asterx && !run_fx10 && !run_gox && !run_lms4xxx) {
        common::Log::LogAndThrow(kModule, "No drivers enabled in the main config", "", /*throw_error=*/false);
        return config_failed();
    }
    common::Log::LogMessage(spdlog::level::info, kModule,
                             std::string(common::Markers::kStartingDrivers) + std::string(run_asterx ? " [AsteRx]" : "")
                             + std::string(run_gox ? " [GoX]" : "") + std::string(run_fx10 ? " [FX10]" : "") +
                             std::string(run_lms4xxx ? " [LMS4xxx]" : ""));

    // Create driver apps behind the unified IDriverApp interface
    // LMS4xxx maps one app per LiDAR instance
    // multiple Go-X cameras are managed inside the single GoxDriverApp (cameras[] in its YAML)
    struct DriverSlot {
        std::unique_ptr<common::IDriverApp> app;
        std::string_view name; // failure-log prefix ("AsteRx"...), GUI maps it to a sensor
        std::string_view init_failed; // common::Markers::k*InitFailed
        std::string_view run_exception; // common::Markers::k*RunException
        std::thread thread;
        std::string key{}; // stable GUI identity, including the configured LiDAR id
    };
    std::vector<DriverSlot> drivers;

    // Uniform bring-up per enabled driver
    // Snapshot the drivers' configs into <data_folder>/config/, then create the apps
    const auto copy_config = [&main_config](const std::filesystem::path &src, std::string_view name) -> bool {
        try {
            std::filesystem::copy_file(
                src, main_config.data_folder_path /
                     fmt::format("config/config-{}_{}.yaml", name, main_config.timestamp),
                std::filesystem::copy_options::none);
            return true;
        } catch (const std::exception &e) {
            common::Log::LogAndThrow(kModule, fmt::format("Cannot copy {} config", name), e.what(),
                                     /*throw_error=*/false);
            return false;
        }
    };

    if (!copy_config(main_config_path, "main")) {
        return config_failed();
    }

    if (run_asterx) {
        // Currently we only support one AsteRx equipment
        if (!copy_config(main_config.asterx_config_path, "asterx")) {
            return config_failed();
        }
        drivers.push_back(
            {
                std::make_unique<AsterxDriverApp>(main_config),
                "AsteRx",
                common::Markers::kAsterxInitFailed,
                common::Markers::kAsterxRunException,
                {}
            }
        );
        drivers_json.AddDriver("asterx", true,
                               main_config.data_folder_path / fmt::format(
                                   "config/config-{}_{}.yaml", "asterx", main_config.timestamp));
        drivers.back().key = "asterx";
    }
    if (run_fx10) {
        // Ordering vs gox is free: Fx10DriverApp::init runs its own idempotent
        // GenICam env bootstrap before the first eBUS SDK call
        if (!copy_config(main_config.fx10_config_path, "fx10")) {
            return config_failed();
        }
        drivers.push_back(
            {
                std::make_unique<Fx10DriverApp>(main_config),
                "FX10",
                common::Markers::kFx10InitFailed,
                common::Markers::kFx10RunException,
                {}
            }
        );
        drivers_json.AddDriver("fx10", true,
                               main_config.data_folder_path / fmt::format(
                                   "config/config-{}_{}.yaml", "fx10", main_config.timestamp));
        drivers.back().key = "fx10";
    }
    if (run_gox) {
        if (!copy_config(main_config.gox_config_path, "gox")) {
            return config_failed();
        }
        drivers.push_back(
            {
                std::make_unique<GoxDriverApp>(main_config),
                "GoX",
                common::Markers::kGoxInitFailed,
                common::Markers::kGoxRunException,
                {}
            }
        );
        drivers_json.AddDriver("gox", true,
                               main_config.data_folder_path / fmt::format(
                                   "config/config-{}_{}.yaml", "gox", main_config.timestamp));
        drivers.back().key = "gox";
    }
    if (run_lms4xxx) {
        const std::string lms4xxx_config_path = main_config.lms4xxx_config_path;
        if (!copy_config(lms4xxx_config_path, "lms4xxx")) {
            return config_failed();
        }
        lms4xxx::AppConfig lms4xxx_config;
        try {
            lms4xxx_config = lms4xxx::LoadAppConfig(lms4xxx_config_path);
        } catch (const lms4xxx::ConfigError &e) {
            common::Log::LogAndThrow(kModule, "LMS4xxx config error", e.what(), /*throw_error=*/false);
            return config_failed();
        }
        for (const auto *lidar: lms4xxx_config.EnabledLidars()) {
            drivers.push_back(
                {
                    std::make_unique<Lms4xxxDriverApp>(main_config, lms4xxx_config, *lidar),
                    "LMS4xxx",
                    common::Markers::kLms4xxxInitFailed,
                    common::Markers::kLms4xxxRunException,
                    {}
                }
            );
            drivers.back().key = "lms:" + lidar->id;
        }
        drivers_json.AddDriver("lms4xxx", true,
                               main_config.data_folder_path / fmt::format(
                                   "config/config-{}_{}.yaml", "lms4xxx", main_config.timestamp));
    }
    if (!drivers_json.WriteRunning()) {
        common::Log::LogMessage(spdlog::level::err, kModule, "Cannot persist run manifest; acquisition not started");
        return 1;
    }

    // Rig-wide guards. Every driver writes under data_folder_path, so one
    // statvfs covers them all; the no-data watchdog polls each driver's own
    // MicrosSinceLastData().
    common::RunGuards guards(main_config.guards);

    // Pre-flight, BEFORE bring-up: Init() is sequential and can take minutes
    // (discovery retries, PTP convergence, a receiver warm-up), and nothing
    // supervises it. Refusing here costs nothing; finding out afterwards costs
    // the session.
    if (const auto verdict = guards.CheckDisk(common::FreeSpaceGiB(main_config.data_folder_path));
        verdict.verdict == common::RunGuards::Verdict::kStop) {
        drivers_json.Finalize("failed (disk)");
        common::Log::LogAndThrow(kModule, fmt::format("{} under '{}' — not starting", verdict.message,
                                                        main_config.data_folder_path.string()));
    }

    // Initialize drivers in creation order
    const auto external_stop = [&drivers] {
        return g_terminate.load(std::memory_order_acquire) ||
               std::ranges::any_of(drivers, [](const DriverSlot &d) { return d.app->HasFailed(); });
    };
    std::size_t initialized = 0;
    bool lifecycle_ok = true;
    auto sensor_states = nlohmann::ordered_json::object();
    for (const auto &d : drivers) sensor_states[d.key] = {{"state", "waiting"}, {"error", ""}};
    const auto publish_lifecycle = [&](const char *phase, bool configuration_read) {
        for (const auto &d : drivers) {
            if (d.app->HasFailed()) {
                sensor_states[d.key] = {{"state", "failed"}, {"error", "Driver reported a failure; see session log"}};
            }
        }
        if (!drivers_json.UpdateLifecycle(phase, configuration_read, sensor_states)) {
            lifecycle_ok = false;
            g_terminate.store(true, std::memory_order_release);
        }
    };
    publish_lifecycle("initializing", false);
    for (auto &d: drivers) {
        if (external_stop()) {
            g_terminate.store(true, std::memory_order_release);
            break;
        }
        bool ready = false;
        try {
            ready = d.app->Init(external_stop);
        } catch (const std::exception &e) {
            d.app->RequestFailure();
            common::Log::LogAndThrow(kModule, d.init_failed, e.what(), /*throw_error=*/false);
        }
        if (!ready) {
            if (external_stop()) {
                g_terminate.store(true, std::memory_order_release);
                common::Log::LogMessage(spdlog::level::warn, kModule,
                                         fmt::format("{} bring-up interrupted, shutting down", d.name));
                break;
            }
            d.app->RequestFailure();
            common::Log::LogAndThrow(kModule, d.init_failed, "", /*throw_error=*/false);
            g_terminate.store(true, std::memory_order_release);
            break; // all partial and initialized drivers are shut down below
        }
        ++initialized;
        sensor_states[d.key]["state"] = "running";
        publish_lifecycle("initializing", initialized == drivers.size());
    }

    // LMS4xxx maps one app per LiDAR instance; the driver-level lifecycle
    // markers are therefore aggregated here (per-instance markers come from
    // each Lms4xxxDriverApp itself).
    const auto lms_ready_count = [&drivers, &initialized] {
        std::size_t n = 0;
        for (std::size_t i = 0; i < initialized; ++i) {
            if (drivers[i].name == "LMS4xxx") {
                ++n;
            }
        }
        return n;
    };
    const auto lms_total = static_cast<std::size_t>(
        std::ranges::count_if(drivers.begin(), drivers.end(),
                              [](const DriverSlot &d) { return d.name == "LMS4xxx"; }));
    if (lms_total > 0 && lms_ready_count() == lms_total) {
        common::Log::LogMessage(spdlog::level::info, common::Markers::kModuleLms4xxx,
                                 common::Markers::kLmsInitialized);
    }

    // Run the successfully initialized drivers concurrently
    for (std::size_t i = 0; i < initialized; ++i) {
        if (g_terminate.load(std::memory_order_acquire)) {
            break;
        }
        auto &d = drivers[i];
        try {
            d.thread = std::thread([&d]() {
                try {
                    d.app->Run();
                } catch (const std::exception &e) {
                    // Never rethrow inside a std::thread (std::terminate).
                    common::Log::LogAndThrow(kModule, d.run_exception, e.what(), /*throw_error=*/false);
                    d.app->RequestFailure();
                }
            });
        } catch (const std::exception &e) {
            d.app->RequestFailure();
            g_terminate.store(true, std::memory_order_release);
            common::Log::LogMessage(spdlog::level::err, kModule,
                                   fmt::format("{} run thread could not start: {}", d.name, e.what()));
            break; // join any already-running drivers and finalize normally below
        }
    }

    // Any driver's termination (or a signal, or a guard) takes the whole rig
    // down together. A guard trip must set first_to_terminate and break — NOT
    // g_terminate: that flag means "the operator stopped us", and going through
    // it would make the run report "completed" and exit 0.
    constexpr std::string_view kDiskGuardName = "disk"; // not any one sensor's fault
    // Latch key per SLOT, not per driver name: LMS4xxx maps one slot per LiDAR,
    // so keying on the name alone would let one silent instance suppress
    // another's warning.
    std::vector<std::string> guard_keys;
    guard_keys.reserve(drivers.size());
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        guard_keys.push_back(fmt::format("{}#{}", drivers[i].name, i));
    }
    std::string_view first_to_terminate;
    bool driver_requested_stop = false;
    if (!g_terminate.load(std::memory_order_acquire)) publish_lifecycle("running", true);
    auto next_disk_check = std::chrono::steady_clock::now();
    auto next_disk_report = next_disk_check; // first tick logs, then once a minute
    while (!g_terminate.load(std::memory_order_acquire)) {
        const auto terminated =
                std::ranges::find_if(drivers.begin(), drivers.end(),
                                     [](DriverSlot &d) {
                                         return d.app->TerminateFlag().load(std::memory_order_acquire);
                                     });
        if (terminated != drivers.end()) {
            first_to_terminate = terminated->name;
            driver_requested_stop = true; // classify after that driver's final close
            break;
        }

        // Disk: one statvfs per second, independent of the poll period.
        if (const auto now = std::chrono::steady_clock::now(); now >= next_disk_check) {
            next_disk_check = now + std::chrono::seconds(1);
            const double free_gib = common::FreeSpaceGiB(main_config.data_folder_path);
            // The per-driver [Statistics] lines no longer carry free space (one
            // filesystem, one number); this is where the log records it, slowly
            // enough not to drown the drivers' own output.
            if (now >= next_disk_report) {
                next_disk_report = now + std::chrono::seconds(60);
                common::Log::LogMessage(spdlog::level::info, kModule,
                                         free_gib >= 0.0
                                             ? fmt::format("Disk free {:.1f} GiB under '{}'", free_gib,
                                                           main_config.data_folder_path.string())
                                             : fmt::format("Disk free unknown under '{}' (statvfs failed)",
                                                           main_config.data_folder_path.string()));
            }
            const auto verdict = guards.CheckDisk(free_gib);
            if (verdict.verdict == common::RunGuards::Verdict::kStop) {
                common::Log::LogMessage(spdlog::level::err, kModule,
                                         fmt::format("{} under '{}' — stopping every driver cleanly",
                                                     verdict.message, main_config.data_folder_path.string()));
                first_to_terminate = kDiskGuardName;
                break;
            }
            if (verdict.verdict == common::RunGuards::Verdict::kWarn) {
                common::Log::LogMessage(spdlog::level::warn, kModule, verdict.message);
            }
        }

        // No-data watchdog, every poll. Only the drivers that finished bring-up
        // are asked; the rest have nothing to be silent about.
        bool watchdog_tripped = false;
        for (std::size_t i = 0; i < initialized; ++i) {
            auto &d = drivers[i];
            const auto verdict = guards.CheckSensor(guard_keys[i], d.app->MicrosSinceLastData());
            if (verdict.verdict == common::RunGuards::Verdict::kStop) {
                // "<DriverName> driver stopped: no data (...)": the leading name
                // is what routes this to the sensor's card in the GUI.
                common::Log::LogMessage(spdlog::level::err, kModule,
                                         fmt::format("{}{} ({})", d.name, common::Markers::kGuardNoDataSuffix,
                                                     verdict.message));
                first_to_terminate = d.name;
                watchdog_tripped = true;
                break;
            }
            if (verdict.verdict == common::RunGuards::Verdict::kWarn) {
                common::Log::LogMessage(spdlog::level::warn, kModule,
                                         fmt::format("{} {}", d.name, verdict.message));
            }
        }
        if (watchdog_tripped) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Propagate termination to all drivers
    publish_lifecycle("stopping", initialized == drivers.size());
    for (auto &d: drivers) {
        d.app->TerminateFlag().store(true, std::memory_order_release);
    }

    if (int sig = g_signal_received.load(std::memory_order_relaxed); sig != 0) {
        common::Log::LogMessage(spdlog::level::warn, kModule,
                                 fmt::format(fmt::runtime(common::Markers::kReceivedSignalTpl), sig));
    }

    // Join run threads, then shut down in reverse creation order
    for (auto &d: drivers) {
        if (d.thread.joinable()) {
            d.thread.join();
        }
    }
    for (auto &driver: std::views::reverse(drivers)) {
        try {
            driver.app->Shutdown();
        } catch (const std::exception &e) {
            driver.app->RequestFailure();
            common::Log::LogMessage(spdlog::level::err, kModule,
                                   fmt::format("{} shutdown failed: {}", driver.name, e.what()));
        }
        sensor_states[driver.key]["state"] = driver.app->HasFailed() ? "failed" : "stopped";
        publish_lifecycle("stopping", initialized == drivers.size());
    }
    std::string failed_at_shutdown;
    for (const auto &driver: drivers) {
        drivers_json.AddDriverResult(std::string(driver.name), driver.app->HasFailed(), driver.app->FinalStatistics());
        if (driver.app->HasFailed()) {
            if (!failed_at_shutdown.empty()) {
                failed_at_shutdown += ", ";
            }
            failed_at_shutdown.append(driver.name);
        }
    }
    if (lms_ready_count() > 0 && failed_at_shutdown.empty()) {
        // Every initialized LiDAR instance has now been shut down
        common::Log::LogMessage(spdlog::level::info, common::Markers::kModuleLms4xxx, common::Markers::kLmsShutdown);
    }

    // Three distinct outcomes, and the exit code carries them:
    //     - a signal is the operator stopping the rig (0);
    //     - a driver that gave up on its own is a FAILED Run (1);
    //     - and reaching the end with neither is a completed Run (0).
    int exit_code = 0;
    bool manifest_ok = false;
    if (!lifecycle_ok) {
        manifest_ok = drivers_json.Finalize("failed (status persistence)");
        exit_code = 1;
    } else if (!failed_at_shutdown.empty()) {
        manifest_ok = drivers_json.Finalize(fmt::format("failed ({})", failed_at_shutdown));
        common::Log::LogMessage(spdlog::level::err, kModule,
                               fmt::format("Run is INCOMPLETE: {} reported a failure during recording or shutdown",
                                           failed_at_shutdown));
        exit_code = 1;
    } else if (!first_to_terminate.empty() && !driver_requested_stop) {
        manifest_ok = drivers_json.Finalize(fmt::format("failed ({})", first_to_terminate));
        common::Log::LogMessage(spdlog::level::err, kModule,
                               fmt::format("Run is INCOMPLETE: {} stopped the rig", first_to_terminate));
        exit_code = 1;
    } else if (const int sig = g_signal_received.load(std::memory_order_relaxed); sig != 0) {
        manifest_ok = drivers_json.Finalize(fmt::format("interrupted (signal {})", sig));
    } else {
        manifest_ok = drivers_json.Finalize("completed");
    }
    if (!manifest_ok) {
        common::Log::LogMessage(spdlog::level::err, kModule, "Final run manifest could not be persisted; integrity result unavailable");
        exit_code = 1;
    }

    if (const auto dropped = common::Logger::ConsoleLinesDropped(); dropped > 0) {
        // Console only: the session log file is complete regardless
        common::Log::LogMessage(spdlog::level::warn, kModule,
                                 fmt::format("{} console log line(s) were dropped because the stderr reader "
                                             "(GUI / docker exec) did not keep up; the session log file is complete",
                                             dropped));
    }
    if (exit_code == 0) {
        common::Log::LogMessage(spdlog::level::info, kModule, common::Markers::kAllDriversShutDown);
    } else {
        common::Log::LogMessage(spdlog::level::err, kModule, "Run failed; recording is INCOMPLETE");
    }
    return exit_code;
}
