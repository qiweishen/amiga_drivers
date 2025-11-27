#include <INIReader.h>
#include <atomic>
#include <boost/date_time.hpp>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <spdlog/fmt/chrono.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "ins401_protocol.h"
#include "ins_discover.h"
#include "ins_receiver.h"
#include "ntrip_client.h"



static std::atomic<bool> g_terminate{ false };


static void SignalHandler(int sig) {
	std::cerr << "\n[Signal] Received signal " << sig << ", shutting down...\n";
	g_terminate.store(true, std::memory_order_release);
}


int main(int argc, char *argv[]) {
	std::signal(SIGINT, SignalHandler);	  // Ctrl+C
	std::signal(SIGTERM, SignalHandler);  // kill
	std::signal(SIGABRT, SignalHandler);  // IDE abort
	std::signal(SIGTSTP, SignalHandler);  // Ctrl+Z
	std::signal(SIGHUP, SignalHandler);	  // Shutdown the terminal

	// ---------------------------------------------------------------------------------------

	// 读取配置文件并建立输出文件夹
	std::string config_path = argc > 1 ? argv[1] : "../Config.ini";
	const INIReader configures(config_path);
	if (configures.ParseError() < 0) {
		fmt::print(stderr, "Cannot load Config.ini file from {}\n", config_path);
		return 1;
	}

	std::string output_folder_path = configures.Get("General", "output_directory", "./data");
	auto now = std::chrono::system_clock::now();
	std::string timestamp = fmt::format("{:%Y%m%d_%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(now));
	std::string data_folder_path = fmt::format("{}/{}", output_folder_path, timestamp);
	std::filesystem::create_directories(output_folder_path);
	std::filesystem::copy_file(config_path, fmt::format("{}/Config_{}.ini", output_folder_path, timestamp),
							   std::filesystem::copy_options::overwrite_existing);

	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	console_sink->set_level(spdlog::level::info);
	auto file_sink =
			std::make_shared<spdlog::sinks::basic_file_sink_mt>(fmt::format("{}/log_{}.log", data_folder_path, timestamp), true);
	file_sink->set_level(spdlog::level::trace);
	std::vector<spdlog::sink_ptr> sinks{ console_sink, file_sink };
	auto logger = std::make_shared<spdlog::logger>("INS401 Driver", sinks.begin(), sinks.end());
	logger->set_level(spdlog::level::trace);
	spdlog::set_default_logger(logger);

	// 发现设备
	auto discover = std::make_unique<INSDeviceDiscover>();
	auto devices = discover->DiscoverDevices();
	if (devices.empty()) {
		return 1;
	}
	const DeviceInfo device = devices.begin()->second;
	spdlog::info("Using {} on interface {} with MAC {}", device.product, device.interface_name, device.mac_address);

	// ---------------------------------------------------------------------------------------

	// 2) 启动接收器线程
	auto receiver_ptr = std::make_shared<INSDeviceReceiver>(
			device.interface_name, device.mac_address, configures.GetBoolean("INS401 Receiver", "save_data", true), data_folder_path);
	std::thread receiver_thread([&receiver_ptr]() {
		try {
			receiver_ptr->Run();
		} catch (const std::exception &e) {
			std::cerr << "[Receiver] exception: " << e.what() << std::endl;
			g_terminate.store(true);
		}
	});
	//
	//
	// // 3) 配置 NTRIP
	// NTRIPClient::Config config;
	// config.host = configures.Get("NTRIP Client", "host", "");
	// config.port = static_cast<int>(configures.GetInteger("NTRIP Client", "port", 8080));
	// config.mount_point = configures.Get("NTRIP Client", "mount_point", "");
	// config.username = configures.Get("NTRIP Client", "username", "");
	// config.password = configures.Get("NTRIP Client", "password", "");
	// config.is_ssl = configures.GetBoolean("NTRIP Client", "use_ssl", false);
	// config.verify_ssl = configures.GetBoolean("NTRIP Client", "verify_ssl", false);
	// config.nmea_gga = configures.Get("NTRIP Client", "nmea_gga", "");
	// auto ntrip_client = std::make_unique<NTRIPClient>(config);
	//
	//
	// // 4) 设置回调，并启动客户端线程
	// auto ntrip_callback = std::make_unique<NTRIP_Callback>(device.interface_name, device.mac_address, device.localhost_mac_address);
	// ntrip_client->SetDataCallback(
	// 		[cb = ntrip_callback.get()](const uint8_t *payload, const size_t len) { cb->SendToINS401(payload, len); });
	// std::thread ntrip_client_thread([&ntrip_client]() {
	// 	try {
	// 		ntrip_client->Connect();
	// 		ntrip_client->StartReceiving();
	// 	} catch (const std::exception &e) {
	// 		std::cerr << "[NTRIP Client] exception: " << e.what() << std::endl;
	// 		g_terminate.store(true);
	// 	}
	// });
	//
	//
	// // 5) 主循环：等待退出事件（信号、错误、或自定义条件）
	// while (!g_terminate.load(std::memory_order_acquire)) {
	// 	// 可在此处检查 ntrip_client 状态、心跳、吞吐、错误码等
	// 	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	// }
	//
	//
	// 6) 退出流程：先停接收器，再 join
	receiver_ptr->Stop();
	if (receiver_thread.joinable()) {
		receiver_thread.join();
	}
	// ntrip_client->Disconnect();
	// if (ntrip_client_thread.joinable()) {
	// 	ntrip_client_thread.join();
	// }
	return 0;
}
