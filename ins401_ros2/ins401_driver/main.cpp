#include <iostream>

#include "ins_discover.h"
#include "ins_receiver.h"
#include "ntrip_client.h"


int main() {
	// // Find INS401 device
	// INSDeviceDiscover discover;
	// std::map<std::string, DeviceInfo> devices = discover.GetDiscoveredDevices();
	// for (const auto& [mac, device]: devices) {
	// 	std::cout << "Discovered INS401 device on interface " << device.interface_name << " with MAC " << device.mac_address << std::endl;
	// }
	// DeviceInfo device = devices.begin()->second;
	// std::cout << "Using device on interface " << device.interface_name << " with MAC " << device.mac_address << std::endl;
	//
	// // Start receiving INS401 data
	// INSDeviceReceiver receiver(device.interface_name, device.mac_address, true);
	// std::thread receiver_thread([&receiver]() {
	// 	receiver.Run();
	// });

	std::string host = "ntrip.data.gnss.ga.gov.au";
	int port = 443;
	bool is_ssl = true;
	std::string username = "TPA_Nav";
	std::string password = "vExnar6pajxexexreh@tpa";
	std::string mountpoint = "ADDE00AUS0";

	// std::string host = "www.smartnetaus.com";
	// int port = 15101;
	// std::string username = "tpa_field";
	// std::string password = "3915";
	// std::string mountpoint = "MSM_VRS";

	NTRIPClient ntrip_client(host, port, is_ssl, username, password, mountpoint);
	ntrip_client.SetRTCMCallback(HandleRTCMMessage);
	if (ntrip_client.Connect()) {
		std::cout << "Connected successfully!" << std::endl;
		// Start receiving RTCM data
		ntrip_client.StartReceiving();
		std::cout << "Started receiving RTCM data..." << std::endl;
		// Monitor connection for a period
		auto start_time = std::chrono::steady_clock::now();
		auto duration = std::chrono::seconds(600);	// Run for 600 seconds

		while (true) {
			auto current_time = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time);
			if (elapsed >= duration) {
				std::cout << "\nTest duration completed." << std::endl;
				break;
			}
			// Check connection status
			if (!ntrip_client.IsConnected()) {
				std::cout << "\nConnection lost!" << std::endl;
				std::cout << "Error: " << ntrip_client.GetLastError() << std::endl;
				break;	// Exit if auto-reconnect is disabled
			}
			// Sleep for 1 second
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		// Stop receiving
		std::cout << "\nStopping data reception..." << std::endl;
		ntrip_client.StopReceiving();

		// Disconnect
		std::cout << "Disconnecting..." << std::endl;
		ntrip_client.Disconnect();
	} else {
		std::cout << "Connection failed!" << std::endl;
		std::cout << "Error: " << ntrip_client.GetLastError() << std::endl;
	}

	return 0;
}
