#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <iomanip>
#include <map>

#include "async_socket.h"
#include "data_type.h"



class INSDeviceDiscover {
public:
	explicit INSDeviceDiscover();
	~INSDeviceDiscover();

	/**
	 * Discover devices on all interfaces
	 * @param discovery_time_ms Time to wait for responses
	 * @return Map of discovered devices (MAC -> DeviceInfo)
	 */
	std::map<std::string, DeviceInfo> DiscoverDevices(int discovery_time_ms = 500);

	/**
	 * Stop discovery process
	 */
	void Stop();

private:
	// Async components
	boost::asio::io_context io_context_;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
	std::vector<std::unique_ptr<AsyncRawSocket>> sockets_;
	std::vector<boost::asio::deadline_timer> timers_;

	// Discovery state
	std::map<std::string, DeviceInfo> discovered_devices_;
	std::mutex devices_mutex_;
	std::atomic<bool> running_{ false };
	std::atomic<int> active_interfaces_{ 0 };

	// Pre-parsed broadcast MAC
	std::array<uint8_t, 6> broadcast_mac_{};

	// Receive buffer for each socket
	std::vector<std::vector<uint8_t>> receive_buffers_;

	// Discovery on single interface
	void DiscoverOnInterface(const std::string &interface, const std::string &local_mac, int discovery_time_ms);

	// Handle received packet
	void HandleReceive(const std::string &interface, const std::string &local_mac, boost::system::error_code ec, const uint8_t *data,
					   size_t length);

	// Parse device response
	bool ParseResponse(const std::string &interface, const std::string &local_mac, const uint8_t *buffer, size_t len);

	// Send discovery ping
	void SendDiscoveryPing(AsyncRawSocket &socket, const std::string &interface, const std::array<uint8_t, 6> &src_mac) const;
};
