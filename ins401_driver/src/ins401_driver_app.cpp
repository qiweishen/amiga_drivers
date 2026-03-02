#include "ins401_driver_app.h"

#include <chrono>
#include <filesystem>
#include <spdlog/spdlog.h>

#include "initialization_monitor.h"
#include "ins401_discover.h"
#include "ins401_receiver.h"
#include "ntrip_client.h"
#include "terminal_spinner.h"
#include "ins401_tool.h"
#include "utility.h"


namespace {
    constexpr std::string_view kModule = "InsDriverApp";
}


InsDriverApp::InsDriverApp(const Common::Config &config) {
    // Store config path from the main config. The actual loading is deferred to init().
    config_path_ = config.ins_config_path;
    config_.data_folder_path = config.data_folder_path;
    config_.timestamp = config.timestamp;
}


InsDriverApp::~InsDriverApp() {
    shutdown();
}


bool InsDriverApp::init() {
    // Resolve default config path relative to executable if none was provided.
    if (config_path_.empty()) {
        std::filesystem::path exe_dir = Common::GetExecutableDir(); // exe_dir + "../../" -> project root
        config_path_ = (exe_dir / "../../ins401_driver/config/config-ins401.yaml").string();
    }

    // Load config and initialize the system.
    try {
        InsTool::LoadConfig(config_path_, config_);
        std::filesystem::copy_file(
            config_path_,
            fmt::format("{}/Config_{}.yaml", config_.data_folder_path, config_.timestamp),
            std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception &e) {
        // Use spdlog::error() directly — log_message(err) would re-throw.
        spdlog::error("[InsDriverApp] Config/init failed: {}", e.what());
        return false;
    }

    // Discover device on the network.
    auto discover = std::make_unique<INSDeviceDiscover>();
    auto devices = discover->DiscoverDevices();
    if (devices.empty()) {
        Common::Log::log_message(spdlog::level::warn, kModule, "No INS401 device found on network");
        return false;
    }
    device_ = std::make_unique<DeviceInfo>(devices.begin()->second);
    Common::Log::log_message(spdlog::level::info, kModule,
                     fmt::format("Found {} on interface {} with MAC {}",
                                 device_->product, device_->interface_name, device_->mac_address));

    // Initialize the static initialization monitor.
    init_monitor_ = std::make_shared<InitializationMonitor>(config_);

    // Start receiver thread.
    receiver_ = std::make_shared<INSDeviceReceiver>(device_->interface_name, device_->mac_address, config_);
    receiver_->SetInitializationMonitor(init_monitor_.get());
    receiver_->SetImuCallback([monitor = init_monitor_](const RawIMUData &imu) {
        monitor->OnImuData(imu);
    });
    if (config_.enable_gnss_checking) {
        receiver_->SetGnssCallback([monitor = init_monitor_](const GNSSSolutionData &gnss) {
            monitor->OnGnssData(gnss);
        });
    }

    receiver_thread_ = std::thread([this]() {
        try {
            receiver_->Run();
        } catch (const std::exception &e) {
            // Use spdlog::error() directly — log_message(err) would re-throw.
            spdlog::error("[InsDriverApp] Receiver exception: {}", e.what());
            terminate_.store(true, std::memory_order_release);
        }
    });

    // Wait for the first GNSS solution and gravity estimate.
    init_monitor_->WaitForFirstGnssAndGravity(std::chrono::seconds(3));

    // Configure NTRIP client.
    ntrip_client_ = std::make_unique<NTRIPClient>(config_);
    if (config_.use_vrs) {
        receiver_->SetNtripClient(ntrip_client_.get());
    }

    // Forward RTCM to device and start the NTRIP client.
    ntrip_callback_ = std::make_unique<NTRIPCallback>(
        device_->interface_name, device_->mac_address, device_->localhost_mac_address);
    ntrip_client_->SetDataCallback(
        [cb = ntrip_callback_.get()](const uint8_t *payload, const size_t len) {
            cb->SendToINS401(payload, len);
        });

    ntrip_thread_ = std::thread([this]() {
        try {
            ntrip_client_->Connect();
            ntrip_client_->StartReceiving();
        } catch (const std::exception &e) {
            if (ntrip_client_->IsRTKRequired()) {
                // Use spdlog::error() directly — log_message(err) would re-throw.
                spdlog::error("[InsDriverApp] NTRIP client exception: {}", e.what());
                terminate_.store(true, std::memory_order_release);
            } else {
                Common::Log::log_message(spdlog::level::warn, kModule,
                                 fmt::format("NTRIP client exception (ignored, RTK not required): {}",
                                             e.what()));
                ntrip_client_->Disconnect();
            }
        }
    });

    Common::Log::log_message(spdlog::level::info, kModule, "INS401 driver initialized");
    return true;
}


void InsDriverApp::run() {
    // Wait for static initialization.
    while (!terminate_.load(std::memory_order_acquire) && !init_monitor_->IsInitialized()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    }

    // Show activity spinner while running.
    TerminalSpinner spinner("./spinner_frames.conf");
    while (!terminate_.load(std::memory_order_acquire) && init_monitor_->IsInitialized()) {
        spinner.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    spinner.Clear();

    // Signal that this driver's run-loop has exited, so the unified main
    // can detect it and propagate shutdown to other drivers.
    terminate_.store(true, std::memory_order_release);
}


void InsDriverApp::shutdown() {
    if (shutdown_called_.exchange(true)) return;

    // Signal termination.
    terminate_.store(true, std::memory_order_release);

    // Stop receiver and join its thread.
    if (receiver_) {
        receiver_->Stop();
    }
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }

    // Disconnect NTRIP and join its thread.
    if (ntrip_client_) {
        ntrip_client_->Disconnect();
    }
    if (ntrip_thread_.joinable()) {
        ntrip_thread_.join();
    }

    // Post-process binary data files into ASCII CSV.
    if (receiver_) {
        receiver_->ProcessBinaryFiles();
        receiver_->LogStatistics();
    }

    Common::Log::log_message(spdlog::level::info, kModule, "INS401 driver shutdown complete");
}


void InsDriverApp::request_shutdown() {
    terminate_.store(true, std::memory_order_release);
}


std::atomic<bool> &InsDriverApp::terminate_flag() {
    return terminate_;
}
