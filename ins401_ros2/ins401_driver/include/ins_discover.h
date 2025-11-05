#pragma once

#include <iomanip>
#include <vector>
#include <thread>
#include <map>
#include <algorithm>
#include <netinet/in.h>
#include <atomic>

#include "data_type.h"



const std::string BROADCAST_MAC = "FF:FF:FF:FF:FF:FF";


class INSDeviceDiscover {
public:
	INSDeviceDiscover();
	~INSDeviceDiscover();

	std::map<std::string, DeviceInfo> GetDiscoveredDevices();
	void ClearDiscoveredDevices();

private:
	int raw_socket_;
	std::array<uint8_t, 6> BROADCAST_MAC_{};
	std::array<uint8_t, 2> COMMAND_START_{};
	std::array<uint8_t, 2> REQUEST_INFO_COMMAND_{};
	std::map<std::string, DeviceInfo> discovered_devices;
	std::atomic<bool> running_{false};

	std::vector<uint8_t> BuildPingPacket(const uint8_t *src_mac);
	bool ParseResponse(const std::string &interface, const uint8_t *buffer, size_t len);
	void ListenResponses(const std::string &interface, int timeout_ms);
	void DiscoverDevices(int discovery_time_ms=500);
};
