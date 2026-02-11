#include <INIReader.h>
#include <atomic>
#include <boost/date_time.hpp>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <map>
#include <memory>
#include <spdlog/fmt/chrono.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "initialization_monitor.h"
#include "ins_discover.h"
#include "ins_receiver.h"
#include "ntrip_client.h"
#include "tool.h"


namespace {
    const std::string_view kModule = "Main";
}


static std::atomic<bool> g_terminate{false};


static void SignalHandler(int sig) {
    // Async-safe signal flag for shutdown.
    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format("Received signal {}, shutting down...", sig));
    g_terminate.store(true, std::memory_order_release);
}


int main(int argc, char *argv[]) {
    std::signal(SIGINT, SignalHandler); // Ctrl+C
    std::signal(SIGTERM, SignalHandler); // kill
    std::signal(SIGABRT, SignalHandler); // IDE abort
    std::signal(SIGTSTP, SignalHandler); // Ctrl+Z
    std::signal(SIGHUP, SignalHandler); // Shutdown the terminal

    // ---------------------------------------------------------------------------------------
    // Load config and prepare output directory.
    std::string config_path = argc > 1 ? argv[1] : "../../Config.ini";
    const INIReader configures(config_path);
    if (configures.ParseError() < 0) {
        Tool::LogMessage(spdlog::level::err, kModule, fmt::format("Cannot load Config.ini file from {}", config_path));
        return 1;
    }

    std::string output_folder_path = configures.Get("General", "output_directory", "./data");
    auto now = std::chrono::system_clock::now();
    std::string timestamp = fmt::format("{:%Y%m%d_%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(now));
    std::string data_folder_path = fmt::format("{}/{}", output_folder_path, timestamp);
    std::filesystem::create_directories(data_folder_path);
    std::filesystem::copy_file(config_path, fmt::format("{}/Config_{}.ini", data_folder_path, timestamp), std::filesystem::copy_options::overwrite_existing);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(fmt::format("{}/log_{}.log", data_folder_path, timestamp), true);
    file_sink->set_level(spdlog::level::trace);
    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("INS401 Driver", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);

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
    auto init_monitor = std::make_shared<InitializationMonitor>(configures);


    // Start receiver thread and capture the first GGA.
    auto receiver_ptr = std::make_shared<INSDeviceReceiver>(
        device.interface_name, device.mac_address, configures.GetBoolean("INS401 Receiver", "save_data", true),
        data_folder_path, configures.GetBoolean("NTRIP Client", "enable_vrs", false));
    // Register IMU and GNSS callbacks for the initialization monitor.
    receiver_ptr->SetImuCallback([monitor = init_monitor](const RawIMUData &imu) {
        monitor->OnImuData(imu);
    });
    receiver_ptr->SetGnssCallback([monitor = init_monitor](const GNSSSolutionData &gnss) {
        monitor->OnGnssData(gnss);
    });
    std::thread receiver_thread([&receiver_ptr]() {
        try {
            receiver_ptr->Run();
        } catch (const std::exception &e) {
            Tool::LogMessage(spdlog::level::err, kModule, fmt::format("Receiver exception: {}", e.what()));
            g_terminate.store(true);
        }
    });

    if (!init_monitor->WaitForFirstGnssAndGravity(std::chrono::seconds(30))) {
        Tool::LogMessage(spdlog::level::warn, kModule, "Timed out waiting for first GNSS fix for gravity initialization");
        g_terminate.store(true, std::memory_order_release);
        receiver_ptr->Stop();
        if (receiver_thread.joinable()) {
            receiver_thread.join();
        }
        return 1;
    }
    Tool::LogMessage(spdlog::level::info, kModule, "First GNSS received, gravity initialized");


    // Configure NTRIP client.
    auto ntrip_client_ptr = std::make_unique<NTRIPClient>(configures);
    receiver_ptr->SetNtripClient(ntrip_client_ptr.get());


    // Forward RTCM to device and start the NTRIP client.
    auto ntrip_callback = std::make_unique<NTRIP_Callback>(device.interface_name, device.mac_address,
                                                           device.localhost_mac_address);
    ntrip_client_ptr->SetDataCallback(
        [cb = ntrip_callback.get()](const uint8_t *payload, const size_t len) { cb->SendToINS401(payload, len); });
    std::thread ntrip_client_thread([&ntrip_client_ptr]() {
        try {
            ntrip_client_ptr->Connect();
            ntrip_client_ptr->StartReceiving();
        } catch (const std::exception &e) {
            Tool::LogMessage(spdlog::level::err, kModule, fmt::format("NTRIP client exception: {}", e.what()));
            g_terminate.store(true);
        }
    });


    // Main loop: wait for termination signal, monitor initialization status.
    bool init_logged = false;
    while (!g_terminate.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (!init_logged && init_monitor->IsInitialized()) {
            init_logged = true;
            auto result = init_monitor->GetResult();
            Tool::LogMessage(spdlog::level::info, kModule,
                             fmt::format("Static initialization complete. Roll={:.4f}deg Pitch={:.4f}deg Yaw={:.4f}deg",
                                         result.roll * 180.0 / M_PI, result.pitch * 180.0 / M_PI,
                                         result.yaw * 180.0 / M_PI));
        }
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
    return 0;
}
