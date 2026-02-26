#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <thread>

#include "initialization_monitor.h"
#include "ins401_discover.h"
#include "ins401_receiver.h"
#include "ntrip_client.h"
#include "terminal_spinner.h"
#include "tool.h"


namespace {
    constexpr std::string_view kModule = "Main";
}


static std::atomic<bool> g_terminate{false};
static std::atomic<int> g_signal_received{0};


static void SignalHandler(int sig) {
    g_signal_received.store(sig, std::memory_order_relaxed);
    g_terminate.store(true, std::memory_order_release);
}


int main(int argc, char *argv[]) {
    std::signal(SIGINT, SignalHandler); // Ctrl+C
    std::signal(SIGTERM, SignalHandler); // kill
    std::signal(SIGABRT, SignalHandler); // IDE abort
    std::signal(SIGTSTP, SignalHandler); // Ctrl+Z
    std::signal(SIGHUP, SignalHandler); // Shutdown the terminal


    // Load config and initialize the system.
    std::string config_path = argc > 1 ? argv[1] : "../../Config.ini";
    Config config;
    Tool::LoadConfig(config_path, config);
    Tool::InitializeSystem(config);
    std::filesystem::copy_file(config_path, fmt::format("{}/Config_{}.ini", config.data_folder_path, config.timestamp),
                               std::filesystem::copy_options::overwrite_existing);


    // Discover device on the network.
    auto discover = std::make_unique<INSDeviceDiscover>();
    auto devices = discover->DiscoverDevices();
    if (devices.empty()) {
        return 1;
    }
    const DeviceInfo device = devices.begin()->second;
    Tool::LogMessage(spdlog::level::info, kModule,
                     fmt::format("Found {} on interface {} with MAC {}", device.product, device.interface_name,
                                 device.mac_address));


    // Initialize the static initialization monitor
    auto init_monitor = std::make_shared<InitializationMonitor>(config);


    // Start receiver thread
    auto receiver_ptr = std::make_shared<INSDeviceReceiver>(device.interface_name, device.mac_address, config);
    receiver_ptr->SetInitializationMonitor(init_monitor.get());
    // Register IMU and/or GNSS callbacks for the initialization monitor
    receiver_ptr->SetImuCallback([monitor = init_monitor](const RawIMUData &imu) {
        monitor->OnImuData(imu);
    });
    if (config.enable_gnss_checking) {
        receiver_ptr->SetGnssCallback([monitor = init_monitor](const GNSSSolutionData &gnss) {
            monitor->OnGnssData(gnss);
        });
    }
    std::thread receiver_thread([&receiver_ptr]() {
        try {
            receiver_ptr->Run();
        } catch (const std::exception &e) {
            Tool::LogMessage(spdlog::level::err, kModule, fmt::format("Receiver exception: {}", e.what()));
            g_terminate.store(true);
        }
    });


    // Wait for the first GNSS solution and gravity estimate to be available from the monitor
    init_monitor->WaitForFirstGnssAndGravity(std::chrono::seconds(3));


    // Configure NTRIP client.
    auto ntrip_client_ptr = std::make_unique<NTRIPClient>(config);
    if (config.use_vrs) {
        receiver_ptr->SetNtripClient(ntrip_client_ptr.get());
    }


    // Forward RTCM to device and start the NTRIP client.
    auto ntrip_callback = std::make_unique<NTRIPCallback>(device.interface_name, device.mac_address,
                                                           device.localhost_mac_address);
    ntrip_client_ptr->SetDataCallback(
        [cb = ntrip_callback.get()](const uint8_t *payload, const size_t len) { cb->SendToINS401(payload, len); });
    std::thread ntrip_client_thread([&ntrip_client_ptr]() {
        try {
            ntrip_client_ptr->Connect();
            ntrip_client_ptr->StartReceiving();
        } catch (const std::exception &e) {
            if (ntrip_client_ptr->IsRTKRequired()) {
                Tool::LogMessage(spdlog::level::err, kModule, fmt::format("NTRIP client exception: {}", e.what()));
                g_terminate.store(true);
            } else {
                Tool::LogMessage(spdlog::level::warn, kModule,
                                 fmt::format("NTRIP client exception (but ignored because RTK not required): {}",
                                             e.what()));
                ntrip_client_ptr->Disconnect();
            }
        }
    });


    // Main loop: wait for static initialization, then show activity spinner
    while (!g_terminate.load(std::memory_order_acquire) && !init_monitor->IsInitialized()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    TerminalSpinner spinner("./spinner_frames.conf");
    while (!g_terminate.load(std::memory_order_acquire)) {
        spinner.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
    spinner.Clear();

    if (int sig = g_signal_received.load(std::memory_order_relaxed); sig != 0) {
        Tool::LogMessage(spdlog::level::warn, kModule, fmt::format("Received signal {}, shutting down...", sig));
    }

    // Shutdown sequence: stop receiver, then join threads.
    receiver_ptr->Stop();
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }
    ntrip_client_ptr->Disconnect();
    if (ntrip_client_thread.joinable()) {
        ntrip_client_thread.join();
    }

    // Post-process binary data files into ASCII CSV
    receiver_ptr->ProcessBinaryFiles();

    // Log receiver statistics summary
    receiver_ptr->LogStatistics();

    return 0;
}
