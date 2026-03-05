
/// @brief Unified entry point for running INS401 and LMS4xxx drivers concurrently
///
/// Supports multiple LiDAR instances from a single YAML config. Each driver
/// runs in its own thread; shutdown is orderly via shared terminate flags

#include <atomic>
#include <iostream>
#include <spdlog/fmt/chrono.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "activity_spinner.h"
#include "data_type.h"
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
		config.run_mode = general["Run Mode"].as<int>(0);
		config.ins_config_path = general["INS401 Driver Config Path"].as<std::string>("./ins401_driver/config/config-ins401.yaml");
		config.lidar_config_path =
				general["LMS4XXX Driver Config Path"].as<std::string>("./lms4xxx_driver/config/config-lms4xxx.yaml");

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
		std::filesystem::create_directories(fmt::format("{}/{}", config.data_folder_path, "bin"));
		std::filesystem::create_directories(fmt::format("{}/{}", config.data_folder_path, "config"));

		// Init logging system
		if (config.enable_logging) {
			const std::string log_file = fmt::format("{}/log_{}.log", config.data_folder_path, config.timestamp);
			Common::Logger::init({ log_file, false }, "AmigaDrivers");
		}
	}
}  // namespace


int main(int argc, char *argv[]) {
	// Install a single signal handler with a shared terminate flag
	static std::atomic<bool> g_terminate{ false };
	static std::atomic<int> g_signal_received{ 0 };
	Common::SignalHandler::install(g_terminate, g_signal_received, { SIGINT, SIGTERM, SIGABRT, SIGTSTP, SIGHUP });


	// Read main config and initialize system (e.g. create timestamped data folder)
	std::filesystem::path exe_dir = Common::GetExecutableDir();	 // exe_dir + "../../" -> project root
	std::string main_config_path = (exe_dir / "../../config/config-main.yaml");
	main_config_path = argc > 1 ? argv[1] : main_config_path;
	Common::Config main_config;
	LoadConfig(main_config_path, main_config);
	InitializeSystem(main_config);


	// Determine which drivers to run
	bool run_ins = false;
	bool run_lidar = false;
	switch (main_config.run_mode) {
		case 0:
			// Default: run both
			run_lidar = true;
			run_ins = true;
			break;
		case 1:
			// Run only INS401
			run_ins = true;
			break;
		case 2:
			// Run only LMS4xxx
			run_lidar = true;
			break;
		default:
			Common::Log::log_message(spdlog::level::warn, kModule,
									 fmt::format("Invalid Run Mode {}, defaulting to both drivers", main_config.run_mode));
			run_ins = true;
			run_lidar = true;
	}
	Common::Log::log_message(
			spdlog::level::info, kModule,
			"Starting Amiga Drivers" + std::string(run_ins ? " [INS401]" : "") + std::string(run_lidar ? " [LMS4xxx]" : ""));


	// Create driver apps
	std::unique_ptr<InsDriverApp> ins_app;
	std::vector<std::unique_ptr<LidarDriverApp>> lidar_apps;

	if (run_ins) {
		// In near future we only support one INS401
		ins_app = std::make_unique<InsDriverApp>(main_config);
	}
	if (run_lidar) {
		std::string lidar_config_path = exe_dir / "../../" / main_config.lidar_config_path;
		auto lidar_configs = LMS4xxxTool::LoadConfigs(lidar_config_path);
		for (auto &cfg: lidar_configs) {
			cfg.data_folder_path = main_config.data_folder_path;
			cfg.timestamp = main_config.timestamp;
			lidar_apps.push_back(std::make_unique<LidarDriverApp>(std::move(cfg)));
		}
		Common::Log::log_message(spdlog::level::info, kModule, fmt::format("Created {} LiDAR instance(s)", lidar_apps.size()));
	}

	// Initialize drivers
	if (ins_app) {
		if (!ins_app->Init()) {
			// Program will be terminated
			Common::Log::log_and_throw(kModule, "INS401 driver initialization failed");
		}
	}
	for (auto it = lidar_apps.begin(); it != lidar_apps.end();) {
		if (!(*it)->init()) {
			// Program will be terminated
			Common::Log::log_and_throw(kModule, "LMS4xxx driver initialization failed");
		}
		++it;
	}


	// Run all drivers concurrently
	std::thread ins_thread;
	std::vector<std::thread> lidar_threads;
	if (ins_app) {
		ins_thread = std::thread([&ins_app]() {
			try {
				ins_app->run();
			} catch (const std::exception &e) {
				// Program will be terminated
				Common::Log::log_and_throw(kModule, "INS401 run() exception: {}", e.what());
			}
		});
	}
	for (auto &app: lidar_apps) {
		lidar_threads.emplace_back([&app]() {
			try {
				app->run();
			} catch (const std::exception &e) {
				Common::Log::log_and_throw(kModule, "LMS4xxx run() exception: {}", e.what());
			}
		});
	}


	// Activity spinner: shows animation during idle periods (1.5s no console output)
	Common::ActivitySpinner spinner((exe_dir/ "../../resource/spinner_frames.conf").string());
	spinner.Attach();

	while (!g_terminate.load(std::memory_order_acquire)) {
		if (ins_app && ins_app->terminate_flag().load(std::memory_order_acquire)) {
			break;
		}
		bool any_lidar_terminated = false;
		for (auto &app: lidar_apps) {
			if (app->TerminateFlag().load(std::memory_order_acquire)) {
				any_lidar_terminated = true;
				break;
			}
		}
		if (any_lidar_terminated)
			break;
		spinner.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	spinner.Detach();
	spinner.Clear();


	// Propagate termination to all drivers
	if (ins_app) {
		ins_app->terminate_flag().store(true, std::memory_order_release);
	}
	for (auto &app: lidar_apps) {
		app->TerminateFlag().store(true, std::memory_order_release);
	}

	if (int sig = g_signal_received.load(std::memory_order_relaxed); sig != 0) {
		Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Received signal {}, shutting down all drivers...", sig));
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


	// Shutdown all drivers
	if (ins_app) {
		ins_app->shutdown();
	}
	for (auto &app: lidar_apps) {
		app->shutdown();
	}


	Common::Log::log_message(spdlog::level::info, kModule, "All drivers shut down");
	return 0;
}
