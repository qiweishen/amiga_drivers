#include <atomic>
#include <csignal>
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
	std::signal(SIGHUP, SignalHandler);	  // 终端关闭


	try {
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
				std::make_unique<INSDeviceReceiver>(device.interface_name, device.mac_address, device.localhost_mac_address, true);
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
		config.host = "ntrip.data.gnss.ga.gov.au";
		config.port = 2101;
		config.is_ssl = false;
		config.username = "TPA_Nav";
		config.password = "vExnar6pajxexexreh@tpa";
		config.mount_point = "5REG00AUS0";
		auto ntrip_client = std::make_unique<NTRIPClient>(config);


		// 4) 设置回调，并启动客户端线程
		auto ntrip_callback =
				std::make_unique<NTRIP_Callback>(device.interface_name, device.mac_address, device.localhost_mac_address);
		ntrip_client->SetDataCallback(
				[cb = ntrip_callback.get()](const uint8_t *payload, size_t len) { cb->SendToINS401(payload, len); });
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
	} catch (const std::exception &e) {
		std::cerr << "[main] exception: " << e.what() << std::endl;
		return 2;
	}
}
