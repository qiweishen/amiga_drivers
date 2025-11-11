#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <ins_discover.h>
#include <iostream>
#include <net/if.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "tool.h"



INSDeviceDiscover::INSDeviceDiscover() : sock_fd_(-1) {
	Tool::Ethernet::ParseMACAddressToUint8(BROADCAST_MAC, broadcast_mac_);
}


INSDeviceDiscover::~INSDeviceDiscover() {
	if (sock_fd_ >= 0) {
		close(sock_fd_);
	}
}


std::map<std::string, DeviceInfo> INSDeviceDiscover::GetDiscoveredDevices() {
	DiscoverDevices();
	return discovered_devices_;
}


bool INSDeviceDiscover::ParseResponse(const std::string &interface, const std::string &mac, const uint8_t *buffer, size_t len) {
	if (len < 60) {
		return false;
	}
	const std::string device_mac = Tool::Ethernet::FormatMacAddress(buffer + 6);
	if (std::memcmp(buffer, broadcast_mac_.data(), 6) == 0) {
		return false;
	}
	uint16_t payload_length = buffer[12] | (buffer[13] << 8);
	if (payload_length < 10) {
		return false;
	}
	size_t payload_offset = 14;
	// Check Aceinna packet header (0x5555)
	if (len < payload_offset + 2) {
		return false;
	}
	if (buffer[payload_offset] != 0x55 || buffer[payload_offset + 1] != 0x55) {
		return false;
	}
	// Check Message ID (0x01cc)
	if (len < payload_offset + 4) {
		return false;
	}
	if (buffer[payload_offset + 2] != REQUEST_INFO_COMMAND_BYTES[0] || buffer[payload_offset + 3] != REQUEST_INFO_COMMAND_BYTES[1]) {
		return false;
	}
	if (len < payload_offset + 8) {
		return false;
	}
	uint32_t aceinna_payload_len = buffer[payload_offset + 4] | (buffer[payload_offset + 5] << 8) |
								   (buffer[payload_offset + 6] << 16) | (buffer[payload_offset + 7] << 24);
	if (len < payload_offset + 10 + aceinna_payload_len) {
		return false;
	}
	uint16_t received_crc = (buffer[payload_offset + 8 + aceinna_payload_len]) | buffer[payload_offset + 9 + aceinna_payload_len] << 8;
	uint16_t calculated_crc = Tool::CRC::CalculateINS401_CRC16(&buffer[payload_offset + 2],
															   6 + aceinna_payload_len	// Message ID(2) + Length(4) + Payload
	);
	if (received_crc != calculated_crc) {
		std::cerr << "CRC mismatch! Received: 0x" << std::hex << received_crc << " Calculated: 0x" << calculated_crc << std::dec
				  << std::endl;
		return false;
	}
	DeviceInfo info;
	info.interface_name = interface;  // Filled in DiscoverDevices()
	info.mac_address = device_mac;
	info.localhost_mac_address = mac;
	if (aceinna_payload_len > 0) {
		std::string device_data((char *) (buffer + payload_offset + 8), aceinna_payload_len);
		if (device_data.find(info.product) != std::string::npos) {
			std::istringstream iss(device_data);
			std::vector<std::string> tokens;
			std::string token;
			while (iss >> token) {
				tokens.push_back(token);
			}
			info.part_number = tokens[1];
			info.serial_number = tokens[2];
			info.hardware_version = tokens[4];
			info.imu_serial_number = tokens[6];
			info.firmware_version = tokens[7] + " " + tokens[8] + " " + tokens[9];
			info.bootloader_version = tokens[11];
			info.imu_firmware_version = tokens[12] + " " + tokens[13] + " " + tokens[14];
			info.gnss_chip_firmware_version = tokens[15] + " " + tokens[16] + " " + tokens[17];
		}
	}
	discovered_devices_[device_mac] = info;
	return true;
}


void INSDeviceDiscover::ListenResponses(const std::string &interface, const std::string &mac, int timeout_ms) {
	int epfd = -1;
	if (!Tool::Ethernet::SetupEpollForFd(sock_fd_, epfd, EPOLLIN)) {
		return;
	}
	Tool::Ethernet::EpollGuard epoll_guard(epfd);

	constexpr int MAX_EVENTS = 4;
	epoll_event events[MAX_EVENTS];
	std::vector<uint8_t> buffer(buffer_size_);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

	while (running_.load()) {
		auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			break;
		}
		size_t wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
		const int nfds = ::epoll_wait(epfd, events, MAX_EVENTS, wait_ms);
		if (nfds < 0) {
			if (errno == EINTR) {
				continue;
			}
			std::cerr << "Error: epoll_wait failed: " << strerror(errno) << std::endl;
			break;
		}
		for (int i = 0; i < nfds; ++i) {
			if (events[i].data.fd != sock_fd_) {
				continue;
			}
			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				std::cerr << "Socket error on " << interface << std::endl;
				continue;
			}
			if (events[i].events & EPOLLIN) {
				ssize_t bytes_read;
				do {
					bytes_read = ::recv(sock_fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
					if (bytes_read > 0) {
						ParseResponse(interface, mac, buffer.data(), bytes_read);
					}
				} while (bytes_read > 0 || (bytes_read < 0 && errno == EINTR));
			}
		}
	}
}


void INSDeviceDiscover::DiscoverDevices(int discovery_time_ms) {
	if (geteuid() != 0 && (getuid() <= 6000 || getuid() >= 6100)) {
		std::cerr << "Warning: This program requires root privileges." << std::endl;
		std::cerr << "         Please run with sudo or use a privileged user account (UID 6000-6100)." << std::endl;
		return;
	}
	const auto interfaces = Tool::Ethernet::GetNetworkInterfaces();
	if (interfaces.empty()) {
		std::cerr << "No active network interfaces found." << std::endl;
		return;
	}
	for (const auto &[interface, mac]: interfaces) {
		std::array<uint8_t, 6> src_mac{};
		Tool::Ethernet::ParseMACAddressToUint8(mac, src_mac);
		if (!Tool::Ethernet::CreateAsyncRawSocket(sock_fd_, interface)) {
			std::cerr << "Failed to initialize socket on " << interface << std::endl;
			continue;
		}
		running_ = true;
		std::thread listener(&INSDeviceDiscover::ListenResponses, this, interface, mac, discovery_time_ms);
		std::vector<uint8_t> ping_packet =
				Tool::Ethernet::BuildPacket(COMMAND_START_BYTES, REQUEST_INFO_COMMAND_BYTES, nullptr, 0, broadcast_mac_, src_mac);
		if (Tool::Ethernet::SendBroadcastPacket(interface, broadcast_mac_, sock_fd_, ping_packet)) {
			listener.join();
		} else {
			running_ = false;
			listener.join();
		}
		close(sock_fd_);
		sock_fd_ = -1;
	}
	if (discovered_devices_.empty()) {
		std::cout << "\nNo devices found." << std::endl;
		std::cout << "\nPossible reasons:" << std::endl;
		std::cout << "    • No IMU devices on the network" << std::endl;
		std::cout << "    • Devices are not in discovery mode" << std::endl;
		std::cout << "    • Firewall blocking broadcast packets" << std::endl;
		std::cout << "    • Devices on different network segment" << std::endl;
	}
}
