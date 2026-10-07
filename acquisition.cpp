#include "acquisition.h"
#include <memory>
#include <stdexcept>
#include <utility>
#include <spdlog/fmt/chrono.h>
#if AMIGA_ENABLE_ASTERX
#include "asterx_driver_app.h"
#endif
#if AMIGA_ENABLE_EBUS
#include "fx10_driver_app.h"
#include "gox_driver_app.h"
#endif
#if AMIGA_ENABLE_LMS4XXX
#include "lms4xxx_driver_app.h"
#endif
#include "driver_markers.h"
#include "drivers_json.h"
#include "logger.h"
#include "sensor_sync_hub.h"
#include "session_runner.h"

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

#if !AMIGA_ENABLE_ASTERX
        if (run_asterx) throw std::invalid_argument("This build excludes AsteRx; disable it in General");
#endif
#if !AMIGA_ENABLE_LMS4XXX
        if (run_lms4xxx) throw std::invalid_argument("This build excludes LMS4xxx; disable it in General");
#endif
#if !AMIGA_ENABLE_EBUS
        if (run_fx10 || run_gox) {
            throw std::invalid_argument("This build excludes eBUS; disable FX10 and GoX in General or use an eBUS-enabled build");
        }
#endif
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
        if (!run_asterx && !run_fx10 && !run_gox && !run_lms4xxx) {
            common::Log::LogMessage(spdlog::level::err, kModule, "No drivers enabled in the main config");
            return fail_before_init("failed (configuration)");
        }
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

#if AMIGA_ENABLE_ASTERX
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
#endif
#if AMIGA_ENABLE_EBUS
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
#endif
#if AMIGA_ENABLE_EBUS
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
#endif
#if AMIGA_ENABLE_LMS4XXX
        if (run_lms4xxx) {
            const std::string lms4xxx_config_path = main_config.lms4xxx_config_path;
            if (!copy_config(lms4xxx_config_path, "lms4xxx")) {
                return fail_before_init("failed (configuration)");
            }
            lms4xxx::AppConfig lms4xxx_config;
            try {
                lms4xxx_config = lms4xxx::LoadAppConfig(lms4xxx_config_path);
            } catch (const std::exception &e) {
                common::Log::LogMessage(spdlog::level::err, kModule, "LMS4xxx config error", e.what());
                return fail_before_init("failed (configuration)");
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
#endif
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
