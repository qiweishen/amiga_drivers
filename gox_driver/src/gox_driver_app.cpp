#include "gox_driver_app.h"

#include <chrono>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <string_view>
#include <thread>

#include "capture_runner.hpp"
#include "app_config.hpp"
#include "logger.h"
#include "signal_stop.hpp"
#include "driver_markers.h"
#include "ebus/env_bootstrap.hpp"
#include "utility.h"


GoxDriverApp::GoxDriverApp(const Common::Config &config) : stop_(std::make_unique<jai::StopController>()) {
    // Actual config loading is deferred to init()
    std::filesystem::path exe_dir = Common::GetExecutableDir(); // exe_dir + "../../" -> project root
    config_path_ = exe_dir / "../../" / config.gox_config_path;
    data_folder_path_ = config.data_folder_path;
}


GoxDriverApp::~GoxDriverApp() {
    shutdown();
}


bool GoxDriverApp::init(const std::function<bool()> &external_stop) {
    // Lenient YAML config load (critical invariants still throw ConfigError)
    jai::AppConfig cfg;
    try {
        cfg = jai::load_config(config_path_);
    } catch (const jai::ConfigError &e) {
        Common::Log::log_and_throw(Common::Markers::kModuleGox, "GoX config error", e.what(), false);
        return false;
    }

    // GenICam environment; must precede the first eBUS SDK call
    jai::ebus::bootstrap_env();

    // Bring-up can block for minutes (discovery retries, PTP convergence), so a watcher
    // thread forwards an external terminate into the StopController
    runner_ = std::make_unique<jai::CaptureRunner>(std::move(cfg), stop_.get());
    std::atomic<bool> bring_up_done{false};
    std::thread watcher([this, &external_stop, &bring_up_done] {
        while (!bring_up_done.load(std::memory_order_acquire)) {
            if (terminate_.load(std::memory_order_acquire) || (external_stop && external_stop())) {
                stop_->request_stop(jai::StopReason::External);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    bool bring_up_ok = false;
    std::exception_ptr bring_up_error;
    try {
        bring_up_ok = runner_->init(data_folder_path_ + "/bin/gox");
    } catch (...) {
        // CaptureRunner::init is documented not to throw, but the watcher must
        // never be destroyed while joinable (std::terminate)
        bring_up_error = std::current_exception();
    }
    bring_up_done.store(true, std::memory_order_release);
    watcher.join();
    if (bring_up_error) {
        std::rethrow_exception(bring_up_error);
    }
    if (!bring_up_ok) {
        if (stop_->stop_requested() && stop_->reason() == jai::StopReason::External) {
            Common::Log::log_message(spdlog::level::warn, Common::Markers::kModuleGox, "GoX bring-up interrupted by shutdown request");
        } else {
            Common::Log::log_and_throw(Common::Markers::kModuleGox, "GoX startup failed",
                                       runner_->last_error(), false);
        }
        return false;
    }

    Common::Log::log_message(spdlog::level::info, Common::Markers::kModuleGox, Common::Markers::kGoxInitialized);
    return true;
}


void GoxDriverApp::run() {
    if (runner_) {
        // External terminate -> StopReason::External
        runner_->monitor_loop([this] { return terminate_.load(std::memory_order_acquire); });
    }
    terminate_.store(true, std::memory_order_release);
}


void GoxDriverApp::shutdown() {
    if (shutdown_called_.exchange(true)) {
        return;
    }

    terminate_.store(true, std::memory_order_release);

    if (!runner_) {
        return;
    }
    const bool clean = runner_->shutdown();
    if (clean) {
        Common::Log::log_message(spdlog::level::info, Common::Markers::kModuleGox, Common::Markers::kGoxShutdown);
    } else {
        Common::Log::log_message(spdlog::level::warn, Common::Markers::kModuleGox,
                                 fmt::format("{} ({})", Common::Markers::kGoxSessionIssues, runner_->last_error()));
    }
}
