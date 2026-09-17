#include "gox_driver_app.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include "capture_runner.h"
#include "app_config.h"
#include "logger.h"
#include "sensor_sync_hub.h"
#include "signal_stop.h"
#include "driver_markers.h"
#include "ebus/env_bootstrap.h"
#include "utility.h"


namespace {
    // App-level module token: this file's error lines drive the GUI health machine
    common::DriverLog g_log{std::string(common::Markers::kModuleGox)};
} // namespace


GoxDriverApp::GoxDriverApp(const common::Config &config) : stop_(std::make_unique<gox::StopController>()) {
    // Actual config loading is deferred to Init()
    std::filesystem::path exe_dir = common::GetExecutableDir(); // exe_dir + "../../" -> project root
    config_path_ = exe_dir / "../../" / config.gox_config_path;
    data_folder_path_ = config.data_folder_path;
    sync_ = config.sensor_sync;
}


GoxDriverApp::~GoxDriverApp() {
    Shutdown();
}


bool GoxDriverApp::Init(const std::function<bool()> &external_stop) {
    gox::AppConfig cfg;
    try {
        cfg = gox::LoadAppConfig(config_path_);
    } catch (const gox::ConfigError &e) {
        g_log.Error("GoX config error: {}", e.what());
        return false;
    }

    // GenICam environment; must precede the first eBUS SDK call
    common::Ebus::BootstrapEnv();

    for (const auto &cam: cfg.cameras) {
        if (cam.enabled) {
            camera_ids_.push_back(cam.id);
        }
    }

    // SensorSync: register every camera with the rig's shared session BEFORE any
    // camera is armed (registering opens the board and STOPs whatever it was doing).
    // The pulse rate is the camera's frame_rate_hz (config-checked to fit the JAI
    // pair); a freerun camera asks for no pulses and only has its strobes logged.
    if (cfg.sensor_trigger.enabled) {
        if (!sync_) {
            g_log.Error("GoX startup failed: sensor_trigger.enabled but the host provides no SensorSync session "
                        "(common::Config::sensor_sync is empty)");
            return false;
        }
        for (const auto &cam: cfg.cameras) {
            if (!cam.enabled) {
                continue;
            }
            const bool external = cam.acquisition.trigger.mode == gox::TriggerMode::kExternal;
            if (!external) {
                g_log.Warn("[{}] sensor_trigger is enabled but acquisition.trigger.mode is freerun — no pulses are "
                           "requested for this camera; the timing log still records its exposure strobes", cam.id);
            }
            const double pulse_hz = external ? cam.acquisition.frame_rate_hz.value_or(0.0) : 0.0;
            try {
                sync_->Register(gox::SensorSyncOwner(cam.id), cam.acquisition.trigger.sensor_channel, pulse_hz);
            } catch (const common::SensorSyncError &e) {
                g_log.Error("GoX startup failed: {}", e.what());
                return false;
            }
        }
    } else {
        for (const auto &cam: cfg.cameras) {
            if (cam.enabled && cam.acquisition.trigger.mode == gox::TriggerMode::kExternal) {
                // Somebody else's pulses: silence is not watched and no strobe timing is
                // logged, so the frames' only absolute time is PTP (if enabled)
                g_log.Warn("[{}] external trigger without sensor_trigger: the pulse source is not this process; "
                           "silence is not watched and no SensorSync strobe timing is recorded", cam.id);
            }
        }
    }

    // Bring-up can block for minutes (discovery retries, PTP convergence), so a watcher
    // thread forwards an external terminate into the StopController
    runner_ = std::make_unique<gox::CaptureRunner>(std::move(cfg), stop_.get(), sync_);
    std::atomic<bool> bring_up_done{false};
    std::thread watcher([this, &external_stop, &bring_up_done] {
        while (!bring_up_done.load(std::memory_order_acquire)) {
            if (terminate_.load(std::memory_order_acquire) || (external_stop && external_stop())) {
                stop_->RequestStop(gox::StopReason::kExternal);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    bool bring_up_ok = false;
    std::exception_ptr bring_up_error;
    try {
        bring_up_ok = runner_->Init(data_folder_path_ + "/gox");
    } catch (...) {
        // CaptureRunner::Init is documented not to throw, but the watcher must
        // never be destroyed while joinable (std::terminate)
        bring_up_error = std::current_exception();
    }
    bring_up_done.store(true, std::memory_order_release);
    watcher.join();
    if (bring_up_error) {
        std::rethrow_exception(bring_up_error);
    }
    if (!bring_up_ok) {
        if (stop_->StopRequested() && stop_->Reason() == gox::StopReason::kExternal) {
            g_log.Warn("GoX bring-up interrupted by shutdown request");
            runner_.reset(); // Init already unwound; interruption is not a recording error
        } else {
            g_log.Error("GoX startup failed: {}", runner_->LastError());
        }
        return false;
    }

    for (const auto &id: camera_ids_) {
        g_log.Info(fmt::runtime(common::Markers::kGoxInitializedInstTpl), id);
    }
    g_log.Info("{}", common::Markers::kGoxInitialized);
    return true;
}


void GoxDriverApp::Run() {
    if (runner_) {
        // External terminate -> StopReason::kExternal
        runner_->MonitorLoop([this] { return terminate_.load(std::memory_order_acquire); });
    }
    terminate_.store(true, std::memory_order_release);
}


std::optional<std::uint64_t> GoxDriverApp::MicrosSinceLastData() const {
    // runner_ is created once in Init() and never replaced, and its session list
    // is stable between Init() and Shutdown() — which Main only calls after it
    // has joined the run thread. The read itself touches one atomic per camera.
    if (!runner_ || shutdown_called_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return runner_->MicrosSinceLastData();
}


void GoxDriverApp::Shutdown() {
    if (shutdown_called_.exchange(true)) {
        return;
    }

    terminate_.store(true, std::memory_order_release);

    if (!runner_) {
        return;
    }
    const bool clean = runner_->Shutdown();
    if (clean) {
        for (const auto &id: camera_ids_) {
            g_log.Info(fmt::runtime(common::Markers::kGoxShutdownInstTpl), id);
        }
        g_log.Info("{}", common::Markers::kGoxShutdown);
    } else {
        MarkFailed();
        g_log.Error("{} ({})", common::Markers::kGoxSessionIssues, runner_->LastError());
    }
}
