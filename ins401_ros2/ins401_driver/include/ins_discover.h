#pragma once

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <map>
#include <netinet/in.h>

#include "data_type.h"



class INSDeviceDiscover {
public:
	explicit INSDeviceDiscover();
	~INSDeviceDiscover();

	std::map<std::string, DeviceInfo> GetDiscoveredDevices();

private:
	int sock_fd_{};
	const size_t buffer_size_ = { 2 * 1024 };
	std::array<uint8_t, 6> broadcast_mac_{};
	std::map<std::string, DeviceInfo> discovered_devices_;
	std::atomic<bool> running_{ false };

	void ListenResponses(const std::string &interface, const std::string &mac, int timeout_ms);
	bool ParseResponse(const std::string &interface, const std::string &mac, const uint8_t *buffer, size_t len);
	void DiscoverDevices(int discovery_time_ms = 500);
};
