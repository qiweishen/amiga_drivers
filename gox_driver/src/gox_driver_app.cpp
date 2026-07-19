#include "gox_driver_app.h"

#include <chrono>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <string_view>
#include <thread>

#include "capture_runner.hpp"
#include "core/config.hpp"
#include "core/logger.hpp"
#include "core/signal_stop.hpp"
#include "ebus/env_bootstrap.hpp"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "GoXApp";
}


GoxDriverApp::GoxDriverApp(const Common::Config &config) : stop_(std::make_unique<jai::StopController>()) {
	// Store config path from the main config. The actual loading is deferred to init()
	std::filesystem::path exe_dir = Common::GetExecutableDir();	 // exe_dir + "../../" -> project root
	config_path_ = exe_dir / "../../" / config.gox_config_path;
	data_folder_path_ = config.data_folder_path;
}


GoxDriverApp::~GoxDriverApp() {
	shutdown();
}


bool GoxDriverApp::init(const std::function<bool()> &external_stop) {
	// NOTE: error paths must use log_and_throw(..., throw_error=false):
	// log_message(err) would throw. The config copy into <data_folder>/config/
	// is done by the unified main.

	// Strict JSON config load (unknown keys are fatal)
	jai::AppConfig cfg;
	try {
		cfg = jai::load_config(config_path_);
	} catch (const jai::ConfigError &e) {
		Common::Log::log_and_throw(kModule, "GoX config error", e.what(), false);
		return false;
	}

	// Route gox logs into the unified logger (prefix + spinner pre-log hook)
	jai::logger().set_level(cfg.logging.level);
	jai::logger().configure("[GoX]: ", [] { Common::Log::run_pre_log_callback(); });

	// GenICam environment; must precede the first eBUS SDK call
	jai::ebus::bootstrap_env();

	// Bring all enabled cameras up, recording into <data_folder>/bin/gox/
	// (overrides recording.output_dir / session_name from the JSON). Bring-up
	// can block for minutes (discovery retries, PTP convergence), so a watcher
	// thread forwards an external terminate into the StopController.
	runner_ = std::make_unique<jai::CaptureRunner>(std::move(cfg), stop_.get());
	std::atomic<bool> bring_up_done{ false };
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
		bring_up_ok = runner_->init(data_folder_path_ + "/bin/gox", /*validate_only=*/false);
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
			Common::Log::log_message(spdlog::level::warn, kModule, "GoX bring-up interrupted by shutdown request");
		} else {
			Common::Log::log_and_throw(kModule, fmt::format("GoX startup failed (standalone exit code {})", runner_->exit_code()),
									   "", false);
		}
		return false;
	}

	Common::Log::log_message(spdlog::level::info, kModule, "GoX driver initialized");
	return true;
}


void GoxDriverApp::run() {
	if (runner_) {
		// External terminate -> StopReason::External. Any internal stop (link
		// lost, writer I/O failure, low disk, max_frames/max_duration_s) exits
		// the loop too; the store below then propagates it to the unified main
		// so all drivers shut down together.
		runner_->run_until_stop([this] { return terminate_.load(std::memory_order_acquire); });
	}
	terminate_.store(true, std::memory_order_release);
}


void GoxDriverApp::shutdown() {
	if (shutdown_called_.exchange(true)) {
		return;
	}

	// Signal termination
	terminate_.store(true, std::memory_order_release);

	if (!runner_) {
		return;
	}
	const int code = runner_->shutdown();
	if (code == 0) {
		Common::Log::log_message(spdlog::level::info, kModule, "GoX driver shutdown completely");
	} else {
		Common::Log::log_message(spdlog::level::warn, kModule,
								 fmt::format("GoX session ended with issues (standalone exit code {})", code));
	}
}
