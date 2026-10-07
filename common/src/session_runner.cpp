#include "session_runner.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <ranges>
#include <stdexcept>
#include "data_type.h"
#include "driver_markers.h"
#include "drivers_json.h"
#include "logger.h"
#include "run_guards.h"
#include "sensor_sync_hub.h"

namespace common {
namespace {
constexpr std::string_view kModule = Markers::kModuleMain;

// Covers partial Init, thread creation failure and monitor exceptions. Never
// destroy a joinable worker or skip a later driver's rollback.
class Cleanup {
public:
    Cleanup(std::vector<DriverSlot> &drivers, const Config &config)
        : drivers_(drivers), config_(config) {}
    ~Cleanup() { Stop(); }

    void Stop() noexcept {
        if (stopped_) return;
        stopped_ = true;
        for (auto &driver : drivers_) driver.app->TerminateFlag().store(true, std::memory_order_release);
        for (auto &driver : drivers_) {
            if (driver.thread.joinable()) driver.thread.join();
        }
        for (auto &driver : std::views::reverse(drivers_)) {
            try {
                driver.app->Shutdown();
            } catch (const std::exception &error) {
                driver.app->RequestFailure();
                try { Log::LogMessage(spdlog::level::err, kModule,
                                      fmt::format("{} shutdown failed: {}", driver.name, error.what())); }
                catch (...) {}
            } catch (...) {
                driver.app->RequestFailure();
                // Logging must not prevent cleanup of the remaining devices.
                try { Log::LogMessage(spdlog::level::err, kModule,
                                      fmt::format("{} shutdown threw; recording is incomplete", driver.name)); }
                catch (...) {}
            }
        }
        if (config_.sensor_sync) {
            try { config_.sensor_sync->Disarm("session"); } catch (...) {}
        }
    }

private:
    std::vector<DriverSlot> &drivers_;
    const Config &config_;
    bool stopped_ = false;
};

std::string CollectResults(std::vector<DriverSlot> &drivers, DriversJson &manifest,
                           std::vector<bool> &published) {
    std::string failed;
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        auto &driver = drivers[i];
        if (!published[i]) {
            auto statistics = nlohmann::ordered_json::object();
            try {
                statistics = driver.app->FinalStatistics();
                if (!statistics.is_object()) throw std::runtime_error("FinalStatistics must return an object");
            } catch (const std::exception &error) {
                driver.app->RequestFailure();
                statistics = {{"statistics_error", error.what()}};
            } catch (...) {
                driver.app->RequestFailure();
                statistics = {{"statistics_error", "Final driver statistics unavailable"}};
            }
            manifest.AddDriverResult(std::string(driver.name), driver.app->HasFailed(), statistics);
            published[i] = true;
        }
        if (driver.app->HasFailed()) {
            if (!failed.empty()) failed += ", ";
            failed += driver.key;
        }
    }
    return failed;
}

int Execute(std::vector<DriverSlot> &drivers, const Config &main_config,
            DriversJson &drivers_json, std::atomic<bool> &g_terminate,
            std::atomic<int> &g_signal_received, Cleanup &cleanup,
            std::vector<bool> &published) {
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
        common::Log::LogMessage(spdlog::level::err, kModule,
                               fmt::format("{} under '{}' — not starting", verdict.message,
                                           main_config.data_folder_path.string()));
        if (!drivers_json.Finalize("failed (disk)")) {
            common::Log::LogMessage(spdlog::level::err, kModule, "Final run manifest could not be persisted");
        }
        return 1;
    }

    // Initialize drivers in creation order
    const auto external_stop = [&drivers, &g_terminate] {
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
                sensor_states[d.key]["state"] = "failed";
                if (sensor_states[d.key]["error"].get<std::string>().empty()) {
                    sensor_states[d.key]["error"] = "Driver reported a failure; see session log";
                }
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
            common::Log::LogMessage(spdlog::level::err, kModule, d.init_failed, e.what());
        } catch (...) {
            d.app->RequestFailure();
            common::Log::LogMessage(spdlog::level::err, kModule, d.init_failed, "unknown exception");
        }
        if (!ready) {
            if (external_stop()) {
                g_terminate.store(true, std::memory_order_release);
                common::Log::LogMessage(spdlog::level::warn, kModule,
                                         fmt::format("{} bring-up interrupted, shutting down", d.name));
                break;
            }
            d.app->RequestFailure();
            common::Log::LogMessage(spdlog::level::err, kModule, d.init_failed);
            g_terminate.store(true, std::memory_order_release);
            break; // all partial and initialized drivers are shut down below
        }
        ++initialized;
        sensor_states[d.key]["state"] = "running";
        publish_lifecycle("initializing", initialized == drivers.size());
    }

    // What the rig asked of the SensorSync board, in one line, now that every
    // driver has declared its needs (the pulses themselves start once the cameras
    // are armed; the per-driver [TriggerLog] lines above show each registration)
    if (main_config.sensor_sync && main_config.sensor_sync->HasParticipants()) {
        common::Log::LogMessage(spdlog::level::info, kModule,
                                 "SensorSync: " + main_config.sensor_sync->Describe());
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
                    d.app->RequestFailure();
                    try { common::Log::LogMessage(spdlog::level::err, kModule, d.run_exception, e.what()); }
                    catch (...) {} // logging must never escape a worker boundary
                } catch (...) {
                    d.app->RequestFailure();
                    try { common::Log::LogMessage(spdlog::level::err, kModule, d.run_exception, "unknown exception"); }
                    catch (...) {}
                }
                d.app->TerminateFlag().store(true, std::memory_order_release);
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
                // Keep the display name in the diagnostic and the stable slot
                // key in structured state, including separate LiDAR instances.
                common::Log::LogMessage(spdlog::level::err, kModule,
                                         fmt::format("{}{} ({})", d.name, common::Markers::kGuardNoDataSuffix,
                                                     verdict.message));
                first_to_terminate = d.name;
                // Lifecycle no longer comes from parsing this warning line.
                // Attribute the watchdog failure to its structured sensor slot.
                sensor_states[d.key]["error"] = verdict.message;
                d.app->RequestFailure();
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

    cleanup.Stop();
    for (const auto &driver : drivers) {
        sensor_states[driver.key]["state"] = driver.app->HasFailed() ? "failed" : "stopped";
    }
    publish_lifecycle("stopping", initialized == drivers.size());
    // The SensorSync session ended with the first camera teardown; make sure of it
    // and keep its verdict in the log (a failed session already failed its
    // participants, which is what the manifest reports)
    if (main_config.sensor_sync && main_config.sensor_sync->HasParticipants()) {
        main_config.sensor_sync->Disarm("main");
        common::Log::LogMessage(main_config.sensor_sync->Ok() ? spdlog::level::info : spdlog::level::err, kModule,
                                 "SensorSync session summary: " + main_config.sensor_sync->Summary().dump());
    }
    const auto failed_at_shutdown = CollectResults(drivers, drivers_json, published);
    publish_lifecycle("stopping", initialized == drivers.size());
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
                                             "(GUI) did not keep up; the session log file is complete",
                                             dropped));
    }
    if (exit_code == 0) {
        common::Log::LogMessage(spdlog::level::info, kModule, common::Markers::kAllDriversShutDown);
    } else {
        common::Log::LogMessage(spdlog::level::err, kModule, "Run failed; recording is INCOMPLETE");
    }
    return exit_code;
}
} // namespace

int RunSession(std::vector<DriverSlot> drivers, const Config &config,
               DriversJson &manifest, std::atomic<bool> &terminate,
               std::atomic<int> &signal_received) {
    Cleanup cleanup(drivers, config);
    std::vector<bool> published(drivers.size(), false);
    try {
        return Execute(drivers, config, manifest, terminate, signal_received, cleanup, published);
    } catch (...) {
        cleanup.Stop();
        // Preserve every available result even when a monitor/status callback
        // failed. Already published results must not be appended twice.
        try { CollectResults(drivers, manifest, published); } catch (...) {}
        Log::LogMessage(spdlog::level::err, kModule, "Session coordinator failed; recording is incomplete");
        if (!manifest.Finalize("failed (session coordinator)")) {
            Log::LogMessage(spdlog::level::err, kModule, "Final run manifest could not be persisted");
        }
        return 1;
    }
}
} // namespace common
