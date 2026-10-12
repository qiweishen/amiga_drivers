#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/fmt/chrono.h>

#include "asterx_driver_app.h"
#include "data_type.h"
#include "driver_markers.h"
#include "drivers_json.h"
#include "fx10_driver_app.h"
#include "gox_driver_app.h"
#include "lms4xxx_driver_app.h"
#include "lms4xxx_driver/include/driver.h"
#include "logger.h"
#include "main_config.h"
#include "sensor_sync_hub.h"
#include "session_runner.h"
#include "signal_handler.h"
#include "utility.h"

#ifndef AMIGA_DRIVERS_VERSION
#define AMIGA_DRIVERS_VERSION "unknown"
#endif
#ifndef AMIGA_DRIVERS_GIT_SHA
#define AMIGA_DRIVERS_GIT_SHA "unknown"
#endif
namespace {
    constexpr std::string_view kModule = common::Markers::kModuleMain;
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

    void StopConfiguredLidars(const lms4xxx::AppConfig &config, const std::atomic<bool> &terminate) {
        // A previous run may have left even a disabled LiDAR measuring. Do this
        // before constructing any driver, including AsteRx and its warm-up gate.
        for (const auto &lidar : config.lidars) {
            if (terminate.load(std::memory_order_acquire)) return;
            // One bounded connection attempt, login, then LMCstopmeas.
            // Unreachable devices are quiet and never prevent rig bring-up.
            (void)lms4xxx::StopMeasurement(lidar.device, config.network);
        }
    }

    int RunAcquisition(common::Config main_config, const std::filesystem::path &main_config_path,
                       std::atomic<bool> &g_terminate, std::atomic<int> &g_signal_received) {
        InitializeSystem(main_config);
        // One SensorSync-Logger board per rig (its port comes from this config): the
        // cameras it triggers (FX10, Go-X) share this session. Each registers during its
        // Init and arms once its camera accepts triggers; the pulses start when every
        // registered camera has armed and stop with the first teardown. The board is
        // not touched unless a driver registers.
        main_config.sensor_sync = std::make_shared<common::SensorSyncHub>(
            main_config.data_folder_path / "sensor_trigger.log", main_config.sensor_trigger_port);

        // Run-level summary at the data folder root; finalized on every exit path below
        common::DriversJson drivers_json(main_config.data_folder_path / "drivers.json");
        drivers_json.SetMeta(main_config.operator_name, main_config.field_name);
        drivers_json.SetRun(main_config.timestamp, main_config.output_directory);
        drivers_json.SetVersion(AMIGA_DRIVERS_VERSION, AMIGA_DRIVERS_GIT_SHA);


        try {
            // Determine which drivers to Run (per-driver enable switches)
            const bool run_asterx = main_config.enable_asterx;
            const bool run_fx10 = main_config.enable_fx10;
            const bool run_gox = main_config.enable_gox;
            const bool run_lms4xxx = main_config.enable_lms4xxx;

            // A configuration that cannot even be snapshotted ends the run before any
            // driver is created: a logged failure and exit code 1, not an uncaught
            // exception (abort, 134) that the GUI can only report as "crashed"
            const auto fail_before_init = [&drivers_json](const char *status) {
                if (!drivers_json.Finalize(status)) {
                    common::Log::LogMessage(spdlog::level::err, kModule,
                                           "Final run manifest could not be persisted; integrity result unavailable");
                }
                return 1;
            };
            common::Log::LogMessage(spdlog::level::info, kModule,
                                     std::string(common::Markers::kStartingDrivers) + std::string(run_asterx ? " [AsteRx]" : "")
                                     + std::string(run_gox ? " [GoX]" : "") + std::string(run_fx10 ? " [FX10]" : "") +
                                     std::string(run_lms4xxx ? " [LMS4xxx]" : ""));

            // Create driver apps behind the unified IDriverApp interface
            // LMS4xxx maps one app per LiDAR instance
            // multiple Go-X cameras are managed inside the single GoxDriverApp (cameras[] in its YAML)
            std::vector<common::DriverSlot> drivers;

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
                    common::Log::LogMessage(spdlog::level::err, kModule, fmt::format("Cannot copy {} config", name), e.what());
                    return false;
                }
            };

            if (!copy_config(main_config_path, "main")) {
                return fail_before_init("failed (configuration)");
            }

            // The startup stop uses every configured endpoint, independently of
            // both the main LMS switch and each lidar.enabled acquisition switch.
            lms4xxx::AppConfig lms4xxx_config;
            try {
                lms4xxx_config = lms4xxx::LoadAppConfig(main_config.lms4xxx_config_path.string(), false);
            } catch (const std::exception &e) {
                common::Log::LogMessage(spdlog::level::err, kModule, "LMS4xxx config error", e.what());
                return fail_before_init("failed (configuration)");
            }
            if (!copy_config(main_config.lms4xxx_config_path, "lms4xxx")) {
                return fail_before_init("failed (configuration)");
            }
            StopConfiguredLidars(lms4xxx_config, g_terminate);
            if (g_terminate.load(std::memory_order_acquire)) {
                const auto status = fmt::format("interrupted (signal {})",
                                               g_signal_received.load(std::memory_order_relaxed));
                if (!drivers_json.Finalize(status)) {
                    common::Log::LogMessage(spdlog::level::err, kModule, "Final run manifest could not be persisted");
                    return 1;
                }
                return 0;
            }
            if (!run_asterx && !run_fx10 && !run_gox && !run_lms4xxx) {
                common::Log::LogMessage(spdlog::level::err, kModule, "No drivers enabled in the main config");
                return fail_before_init("failed (configuration)");
            }
            if (run_lms4xxx && lms4xxx_config.EnabledLidars().empty()) {
                common::Log::LogMessage(spdlog::level::err, kModule,
                                       "LMS4xxx config error: at least one entry must be enabled for acquisition");
                return fail_before_init("failed (configuration)");
            }

            if (run_asterx) {
                // Currently we only support one AsteRx equipment
                if (!copy_config(main_config.asterx_config_path, "asterx")) {
                    return fail_before_init("failed (configuration)");
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
                    return fail_before_init("failed (configuration)");
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
                    return fail_before_init("failed (configuration)");
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


            return common::RunSession(std::move(drivers), main_config, drivers_json, g_terminate, g_signal_received);
        } catch (const std::exception &error) {
            common::Log::LogMessage(spdlog::level::err, kModule, "Cannot prepare acquisition", error.what());
            if (!drivers_json.Finalize("failed (configuration)")) {
                common::Log::LogMessage(spdlog::level::err, kModule, "Final run manifest could not be persisted");
            }
            return 1;
        }
    }
} // namespace

int main(int argc, char *argv[]) {
    static std::atomic<bool> terminate{false};
    static std::atomic<int> signal_received{0};
    common::SignalHandler::Install(terminate, signal_received, {SIGINT, SIGTERM, SIGHUP});
    std::signal(SIGPIPE, SIG_IGN);
    try {
        const auto root = common::GetAbsolutePath(common::GetExecutableDir() / "../../");
        auto path = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("config/config-main.yaml");
        if (path.empty()) {
            throw std::invalid_argument("main config path must not be empty");
        }
        path = common::GetAbsolutePath(path.is_relative() ? root / path : path);
        return RunAcquisition(common::LoadMainConfig(path, root), path, terminate, signal_received);
    } catch (const std::exception &error) {
        std::cerr << "Cannot initialize acquisition session: " << error.what() << '\n';
        return 1;
    }
}
