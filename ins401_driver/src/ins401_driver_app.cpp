#include "ins401_driver_app.h"

#include <chrono>
#include <filesystem>
#include <spdlog/spdlog.h>

#include "driver_markers.h"
#include "initialization_monitor.h"
#include "ins401_discover.h"
#include "ins401_ntrip_client.h"
#include "ins401_receiver.h"
#include "ins401_tool.h"
#include "logger.h"
#include "thread_util.h"
#include "utility.h"

// The app class itself stays global (unified-main convention); its members
// and locals are INS401 types
using namespace INS401;


namespace {
	constexpr std::string_view kModule = Common::Markers::kModuleIns401;
	Common::DriverLog g_log{ std::string(kModule) };
}


Ins401DriverApp::Ins401DriverApp(const Common::Config &config) {
	// Store config path from the main config. The actual loading is deferred to init()
	std::filesystem::path exe_dir = Common::GetExecutableDir();	 // exe_dir + "../../" -> project root
	config_path_ = exe_dir / "../../" / config.ins401_config_path;
	config_.data_folder_path = config.data_folder_path;
	config_.timestamp = config.timestamp;
}


Ins401DriverApp::~Ins401DriverApp() {
	shutdown();
}


bool Ins401DriverApp::init(const std::function<bool()> & /*external_stop*/) {
	// Load config (the copy into <data_folder>/config/ is done by the unified main)
	try {
		Tool::LoadConfig(config_path_, config_);
	} catch (const std::exception &e) {
		Common::Log::log_and_throw(kModule, "Config/init failed", e.what());
	}

	auto discover = std::make_unique<INSDeviceDiscover>();
	auto devices = discover->DiscoverDevices();
	if (devices.empty()) {
		g_log.warn("No INS401 device found on network");
		return false;
	}
	// In our setup, we only support one INS device
	// If multiple are found, just take the first one
	device_ = std::make_unique<DeviceInfo>(devices.begin()->second);
	g_log.info("Found {} on interface {} with MAC {}", device_->product, device_->interface_name, device_->mac_address);

	init_monitor_ = std::make_shared<InitializationMonitor>(config_);

	receiver_ = std::make_shared<INSDeviceReceiver>(device_->interface_name, device_->mac_address, config_);
	receiver_->SetInitializationMonitor(init_monitor_.get());
	receiver_->SetImuCallback([monitor = init_monitor_](const RawIMUData &imu) { monitor->OnImuData(imu); });
	if (config_.enable_gnss_checking) {
		receiver_->SetGnssCallback([monitor = init_monitor_](const GNSSSolutionData &gnss) { monitor->OnGnssData(gnss); });
	}

	receiver_thread_ = std::thread([this]() {
		try {
			receiver_->Run();
		} catch (const std::exception &e) {
			g_log.error("Receiver exception: {}", e.what());
			terminate_.store(true, std::memory_order_release);
		}
	});

	init_monitor_->WaitForFirstGnssAndGravity(std::chrono::seconds(3));

	ntrip_client_ = std::make_unique<NTRIPClient>(config_);
	if (config_.use_vrs) {
		receiver_->SetNtripClient(ntrip_client_.get());
	}

	// Forward RTCM to device only when RTK is required
	if (config_.enable_rtk) {
		ntrip_callback_ =
				std::make_unique<NTRIPCallback>(device_->interface_name, device_->mac_address, device_->localhost_mac_address);
		ntrip_client_->SetDataCallback(
				[cb = ntrip_callback_.get()](const uint8_t *payload, const size_t len) { cb->SendToINS401(payload, len); });
	}

	ntrip_thread_ = std::thread([this]() {
		try {
			if (!ntrip_client_->Connect()) {
				if (config_.enable_rtk) {
					g_log.error("NTRIP connection failed");
					terminate_.store(true, std::memory_order_release);
					return;
				}
				g_log.warn("NTRIP connection failed (RTK not required, ignored)");
				return;
			}
			ntrip_client_->StartReceiving();
		} catch (const std::exception &e) {
			if (config_.enable_rtk) {
				g_log.error("NTRIP client exception: {}", e.what());
				terminate_.store(true, std::memory_order_release);
			} else {
				g_log.warn("NTRIP client exception (RTK not required, ignored): {}", e.what());
				ntrip_client_->Disconnect();
			}
		}
	});

	Common::Log::log_message(spdlog::level::info, kModule, Common::Markers::kIns401Initialized);
	return true;
}


void Ins401DriverApp::run() {
	// Block until termination is requested (spinner is now managed by main)
	Common::ThreadUtil::WaitUntilTerminated(terminate_);
}


void Ins401DriverApp::shutdown() {
	if (shutdown_called_.exchange(true)) {
		return;
	}

	terminate_.store(true, std::memory_order_release);

	if (receiver_) {
		receiver_->Stop();
	}
	if (receiver_thread_.joinable()) {
		receiver_thread_.join();
	}

	// Join the ntrip bring-up thread BEFORE Disconnect: it may still be inside
	// Connect()'s blocking handshake on the TcpClient that Disconnect destroys
	if (ntrip_thread_.joinable()) {
		ntrip_thread_.join();
	}
	if (ntrip_client_) {
		ntrip_client_->Disconnect();
	}

	if (receiver_) {
		receiver_->LogStatistics();
	}

	Common::Log::log_message(spdlog::level::info, kModule, Common::Markers::kIns401Shutdown);
}
