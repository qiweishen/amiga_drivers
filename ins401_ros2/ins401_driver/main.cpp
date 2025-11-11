#include <INIReader.h>
#include <atomic>
#include <boost/date_time.hpp>
#include <csignal>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <iostream>
#include <map>
#include <memory>
#include <thread>

#include "data_type.h"
#include "ins_discover.h"
#include "ins_receiver.h"
#include "ntrip_client.h"



static std::atomic<bool> g_terminate{ false };


static void SignalHandler(int sig) {
	std::cerr << "\n[Signal] Received signal " << sig << ", shutting down...\n";
	g_terminate.store(true, std::memory_order_release);
}


int main() {
	std::signal(SIGINT, SignalHandler);	  // Ctrl+C
	std::signal(SIGTERM, SignalHandler);  // kill
	std::signal(SIGABRT, SignalHandler);  // IDE abort
	std::signal(SIGTSTP, SignalHandler);  // Ctrl+Z
	std::signal(SIGHUP, SignalHandler);	  // Shutdown the terminal

	// 0) 建立输出文件夹并读取配置文件
	auto now = std::chrono::system_clock::now();
	std::string timestamp = fmt::format("{:%Y%m%d_%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(now));
	std::string output_folder_path = fmt::format("./data/{}", timestamp);
	std::filesystem::create_directories(output_folder_path);

	std::string config_path = fmt::format("{}/Config_{}.ini", output_folder_path, timestamp);
	std::filesystem::copy_file("../Config.ini", config_path, std::filesystem::copy_options::overwrite_existing);
	INIReader configures(config_path);
	if (configures.ParseError() < 0) {
		std::cerr << "Cannot load 'Config.ini'" << std::endl;
		return 1;
	}


	// 1) 发现设备
	auto discover = std::make_unique<INSDeviceDiscover>();
	auto devices = discover->GetDiscoveredDevices();
	if (devices.empty()) {
		return 1;
	}
	const DeviceInfo device = devices.begin()->second;
	std::cout << "Using device on interface " << device.interface_name << " with MAC " << device.mac_address << std::endl;


	// 2) 启动接收器线程
	auto receiver =
			std::make_unique<INSDeviceReceiver>(device.interface_name, device.mac_address, device.localhost_mac_address,
												configures.GetBoolean("INS401 Receiver", "save_data", true), output_folder_path);
	std::thread receiver_thread([&receiver]() {
		try {
			receiver->Run();
		} catch (const std::exception &e) {
			std::cerr << "[Receiver] exception: " << e.what() << std::endl;
			g_terminate.store(true);
		}
	});


	// 3) 配置 NTRIP
	NTRIPClient::Config config;
	config.host = configures.Get("NTRIP Client", "host", "");
	config.port = static_cast<int>(configures.GetInteger("NTRIP Client", "port", 8080));
	config.mount_point = configures.Get("NTRIP Client", "mount_point", "");
	config.username = configures.Get("NTRIP Client", "username", "");
	config.password = configures.Get("NTRIP Client", "password", "");
	config.is_ssl = configures.GetBoolean("NTRIP Client", "use_ssl", false);
	config.verify_ssl = configures.GetBoolean("NTRIP Client", "verify_ssl", false);
	config.nmea_gga = configures.Get("NTRIP Client", "nmea_gga", "");
	auto ntrip_client = std::make_unique<NTRIPClient>(config);


	// 4) 设置回调，并启动客户端线程
	auto ntrip_callback = std::make_unique<NTRIP_Callback>(device.interface_name, device.mac_address, device.localhost_mac_address);
	ntrip_client->SetDataCallback(
			[cb = ntrip_callback.get()](const uint8_t *payload, const size_t len) { cb->SendToINS401(payload, len); });
	std::thread ntrip_client_thread([&ntrip_client]() {
		try {
			ntrip_client->Connect();
			ntrip_client->StartReceiving();
		} catch (const std::exception &e) {
			std::cerr << "[NTRIP Client] exception: " << e.what() << std::endl;
			g_terminate.store(true);
		}
	});


	// 5) 主循环：等待退出事件（信号、错误、或自定义条件）
	while (!g_terminate.load(std::memory_order_acquire)) {
		// 可在此处检查 ntrip_client 状态、心跳、吞吐、错误码等
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}


	// 6) 退出流程：先停接收器，再 join
	receiver->Stop();
	if (receiver_thread.joinable()) {
		receiver_thread.join();
	}
	ntrip_client->Disconnect();
	if (ntrip_client_thread.joinable()) {
		ntrip_client_thread.join();
	}
	return 0;
}
