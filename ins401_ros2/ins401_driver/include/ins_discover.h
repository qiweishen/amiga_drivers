#pragma once

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <map>
#include <netinet/in.h>
#include <vector>

#include "data_type.h"



class INSDeviceDiscover {
public:
	INSDeviceDiscover();
	~INSDeviceDiscover();

	std::map<std::string, DeviceInfo> GetDiscoveredDevices();
	void ClearDiscoveredDevices();

private:
	int socket_fd_;
	const size_t buffer_size_ = { 2 * 1024 };
	std::array<uint8_t, 6> broadcast_mac_{};
	std::array<uint8_t, 2> command_start_{};
	std::array<uint8_t, 2> request_info_command_{};
	std::map<std::string, DeviceInfo> discovered_devices;
	std::atomic<bool> running_{ false };

	std::vector<uint8_t> BuildPingPacket(const uint8_t *src_mac);
	bool ParseResponse(const std::string &interface, const uint8_t *buffer, size_t len);
	void ListenResponses(const std::string &interface, int timeout_ms);
	void DiscoverDevices(int discovery_time_ms = 500);
};
