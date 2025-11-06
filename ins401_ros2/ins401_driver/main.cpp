#include <iostream>

#include "ins_discover.h"
#include "ins_receiver.h"
#include "ntrip_client.h"



int main() {
	// Find INS401 device
	INSDeviceDiscover discover;
	std::map<std::string, DeviceInfo> devices = discover.GetDiscoveredDevices();
	for (const auto &[mac, device]: devices) {
		std::cout << "Discovered INS401 device on interface " << device.interface_name << " with MAC " << device.mac_address
				  << std::endl;
	}
	DeviceInfo device = devices.begin()->second;
	std::cout << "Using device on interface " << device.interface_name << " with MAC " << device.mac_address << std::endl;

	// Start receiving INS401 data
	INSDeviceReceiver receiver(device.interface_name, device.mac_address, device.localhost_mac_address, true);
	std::thread receiver_thread([&receiver]() { receiver.Run(); });


	NTRIPClient::Config config;
	config.host = "ntrip.data.gnss.ga.gov.au";
	config.port = 2101;
	config.is_ssl = false;
	config.username = "TPA_Nav";
	config.password = "vExnar6pajxexexreh@tpa";
	config.mount_point = "ADDE00AUS0";

	// config.host = "www.smartnetaus.com";
	// config.port = 15101;
	// config.username = "tpa_field";
	// config.password = "3915";
	// config.mountpoint = "MSM_VRS";

	NTRIPClient ntrip_client(config);
	NTRIP_Callback rtcm_sender(device.interface_name, device.mac_address, device.localhost_mac_address);
	ntrip_client.SetCallback([&rtcm_sender](const uint8_t *data, size_t size) { return rtcm_sender.SendToINS401(data, size); });
	std::thread ntrip_thread([&ntrip_client]() {
		ntrip_client.Connect();
		ntrip_client.StartReceiving();
	});

	receiver_thread.join();
	return 0;
}
