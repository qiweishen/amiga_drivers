#include <INIReader.h>
#include <atomic>
#include <boost/date_time.hpp>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <spdlog/fmt/chrono.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

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
    Tool::LogMessage(spdlog::level::warn, kModule, __func__,
                     fmt::format("Received signal {}, shutting down...", sig));
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
        Tool::LogMessage(spdlog::level::err, kModule, __func__,
                         fmt::format("Cannot load Config.ini file from {}", config_path));
        return 1;
    }

    std::string output_folder_path = configures.Get("General", "output_directory", "./data");
    auto now = std::chrono::system_clock::now();
    std::string timestamp = fmt::format("{:%Y%m%d_%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(now));
    std::string data_folder_path = fmt::format("{}/{}", output_folder_path, timestamp);
    std::filesystem::create_directories(data_folder_path);
    std::filesystem::copy_file(config_path, fmt::format("{}/Config_{}.ini", data_folder_path, timestamp),
                               std::filesystem::copy_options::overwrite_existing);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);
    auto file_sink =
            std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                fmt::format("{}/log_{}.log", data_folder_path, timestamp), true);
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
    Tool::LogMessage(spdlog::level::info, kModule, __func__,
                     fmt::format("Using {} on interface {} with MAC {}", device.product, device.interface_name,
                                 device.mac_address));


    // Start receiver thread and capture the first GGA.
    auto receiver_ptr = std::make_shared<INSDeviceReceiver>(
        device.interface_name, device.mac_address, configures.GetBoolean("INS401 Receiver", "save_data", true),
        data_folder_path);
    std::mutex gga_mutex;
    std::condition_variable gga_cv;
    std::string gga_from_device;
    std::atomic<bool> gga_ready{false};
    std::atomic<NTRIPClient *> ntrip_ptr{nullptr};
    receiver_ptr->SetNmeaCallback([&](const std::string &nmea) {
        if (nmea.rfind("$GPGGA,", 0) != 0 && nmea.rfind("$GNGGA,", 0) != 0) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(gga_mutex);
            if (gga_from_device.empty()) {
                gga_from_device = nmea;
                gga_ready.store(true, std::memory_order_release);
            }
        }
        gga_cv.notify_one();
        if (auto *client = ntrip_ptr.load(std::memory_order_acquire)) {
            client->UpdateGgaFromNmea(nmea);
        }
    });
    std::thread receiver_thread([&receiver_ptr]() {
        try {
            receiver_ptr->Run();
        } catch (const std::exception &e) {
            Tool::LogMessage(spdlog::level::err, kModule, __func__,
                             fmt::format("Receiver exception: {}", e.what()));
            g_terminate.store(true);
        }
    });


    // Configure NTRIP and wait for a valid GGA.
    NTRIPClient::Config config;
    config.host = configures.Get("NTRIP Client", "host", "");
    config.port = static_cast<int>(configures.GetInteger("NTRIP Client", "port", 8080));
    config.mount_point = configures.Get("NTRIP Client", "mount_point", "");
    config.username = configures.Get("NTRIP Client", "username", "");
    config.password = configures.Get("NTRIP Client", "password", "");
    config.is_ssl = configures.GetBoolean("NTRIP Client", "use_ssl", false);
    config.verify_ssl = configures.GetBoolean("NTRIP Client", "verify_ssl", false);
    config.nmea_gga = configures.Get("NTRIP Client", "nmea_gga", "");
    {
        std::unique_lock<std::mutex> lock(gga_mutex);
        while (!gga_ready.load(std::memory_order_acquire) && !g_terminate.load(std::memory_order_acquire)) {
            gga_cv.wait_for(lock, std::chrono::milliseconds(200));
        }
        if (!gga_from_device.empty()) {
            config.nmea_gga = gga_from_device;
        }
    }
    auto ntrip_client = std::make_unique<NTRIPClient>(config);
    ntrip_ptr.store(ntrip_client.get(), std::memory_order_release);


    // Forward RTCM to device and start the NTRIP client.
    auto ntrip_callback = std::make_unique<NTRIP_Callback>(device.interface_name, device.mac_address,
                                                           device.localhost_mac_address);
    ntrip_client->SetDataCallback(
        [cb = ntrip_callback.get()](const uint8_t *payload, const size_t len) { cb->SendToINS401(payload, len); });
    std::thread ntrip_client_thread([&ntrip_client]() {
        try {
            ntrip_client->Connect();
            ntrip_client->StartReceiving();
        } catch (const std::exception &e) {
            Tool::LogMessage(spdlog::level::err, kModule, __func__,
                             fmt::format("NTRIP client exception: {}", e.what()));
            g_terminate.store(true);
        }
    });


    // Main loop: wait for termination signal.
    while (!g_terminate.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }


    // Shutdown sequence: stop receiver, then join threads.
    receiver_ptr->Stop();
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }
    ntrip_client->Disconnect();
    if (ntrip_client_thread.joinable()) {
        ntrip_client_thread.join();
    }
    return 0;
}
