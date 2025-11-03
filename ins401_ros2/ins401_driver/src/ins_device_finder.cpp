#include <ins_device_finder.h>



INSDeviceDiscovery::INSDeviceDiscovery() : raw_socket_(-1), BROADCAST_MAC_(new uint8_t[6]),
                                           COMMAND_START_(new uint8_t[2]),
                                           REQUEST_INFO_COMMAND_(new uint8_t[2]),
                                           discovery_running(false) {
	Tool::Ethernet::ParseMACAddressToUint8(BROADCAST_MAC, BROADCAST_MAC_);
	Tool::Ethernet::ConvertUint16ToUint8(COMMAND_START, COMMAND_START_, LSB);
	Tool::Ethernet::ConvertUint16ToUint8(REQUEST_INFO_COMMAND, REQUEST_INFO_COMMAND_, LSB);
}


INSDeviceDiscovery::~INSDeviceDiscovery() {
	if (raw_socket_ >= 0) {
		close(raw_socket_);
	}
}


std::map<std::string, DeviceInfo> INSDeviceDiscovery::GetDiscoveredDevices() {
	DiscoverDevices();
	return discovered_devices;
}


void INSDeviceDiscovery::ClearDiscoveredDevices() {
	discovered_devices.clear();
}


bool INSDeviceDiscovery::CreateRawSocket(const std::string &interface) {
	raw_socket_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (raw_socket_ < 0) {
		std::cerr << "Error: Failed to create raw socket - " << strerror(errno) << std::endl;
		std::cerr << "Note: This program requires root privileges (sudo)" << std::endl;
		return false;
	}

	int flags = fcntl(raw_socket_, F_GETFL, 0);
	fcntl(raw_socket_, F_SETFL, flags | O_NONBLOCK);

	ifreq ifr{};
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);

	if (ioctl(raw_socket_, SIOCGIFINDEX, &ifr) < 0) {
		std::cerr << "Error: Failed to get interface index for " << interface << " - " << strerror(errno) <<
				std::endl;
		close(raw_socket_);
		raw_socket_ = -1;
		return false;
	}

	sockaddr_ll sll{};
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_ifindex = ifr.ifr_ifindex;
	sll.sll_protocol = htons(ETH_P_ALL);

	if (bind(raw_socket_, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0) {
		std::cerr << "Error: Failed to bind to interface " << interface
				<< " - " << strerror(errno) << std::endl;
		close(raw_socket_);
		raw_socket_ = -1;
		return false;
	}

	constexpr int enable = 1;
	setsockopt(raw_socket_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

	return true;
}


// Ethernet Frame:
// [Destination MAC: FF FF FF FF FF FF]  // 6 bytes
// [Source MAC: xx xx xx xx xx xx]       // 6 bytes
// [Length: 0x0A 0x00]                   // 10 bytes (LSB-first)
// [Payload (Aceinna Packet):]           // 10 bytes + padding
//   [Header: 55 55]                     // 2 bytes
//   [Message ID: 01 CC]                 // 2 bytes (PING_TYPE, LSB-first)
//   [Length: 00 00 00 00]               // 4 bytes (payload length is 0)
//   [Payload: (empty)]                  // 0 bytes
//   [Checksum: xx xx]                   // 2 bytes (CRC16)
// [Padding: 00 00 ... ]                 // 36 bytes (Fill to 46 with zero bytes)
// [Frame CRC: xx xx xx xx]              // 4 bytes
std::vector<uint8_t> INSDeviceDiscovery::BuildPingPacket(const uint8_t *src_mac) {
	std::vector<uint8_t> frame;
	frame.insert(frame.end(), BROADCAST_MAC_, BROADCAST_MAC_ + 6);
	frame.insert(frame.end(), src_mac, src_mac + 6);

	std::vector<uint8_t> aceinna_packet;
	aceinna_packet.insert(aceinna_packet.end(), COMMAND_START_, COMMAND_START_ + 2);
	aceinna_packet.insert(aceinna_packet.end(), REQUEST_INFO_COMMAND_, REQUEST_INFO_COMMAND_ + 2);

	std::vector<uint8_t> ping_payload;
	aceinna_packet.push_back(0x00);
	aceinna_packet.push_back(0x00);
	aceinna_packet.push_back(0x00);
	aceinna_packet.push_back(0x00);

	// CRC16 - MSB first
	const uint16_t crc16 = Tool::INS401::CalcCRC(&aceinna_packet[2], aceinna_packet.size() - 2);
	aceinna_packet.push_back(static_cast<uint8_t>((crc16 >> 8) & 0xFF)); // MSB
	aceinna_packet.push_back(static_cast<uint8_t>(crc16 & 0xFF)); // LSB

	auto eth_payload_length = static_cast<uint16_t>(aceinna_packet.size());
	frame.push_back(static_cast<uint8_t>(eth_payload_length & 0xFF));
	frame.push_back(static_cast<uint8_t>((eth_payload_length >> 8) & 0xFF));

	frame.insert(frame.end(), aceinna_packet.begin(), aceinna_packet.end());

	while (frame.size() - 14 < 46) {
		frame.push_back(0x00);
	}

	return frame;
}


bool INSDeviceDiscovery::ParseResponse(const uint8_t *buffer, size_t len) {
	if (len < 60) {
		return false;
	}

	std::string device_mac = Tool::Ethernet::FormatMacAddress(buffer + 6);
	if (memcmp(buffer, BROADCAST_MAC_, 6) == 0) {
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
	if (buffer[payload_offset + 2] != REQUEST_INFO_COMMAND_[0] ||
	    buffer[payload_offset + 3] != REQUEST_INFO_COMMAND_[1]) {
		return false;
	}

	if (len < payload_offset + 8) {
		return false;
	}
	uint32_t aceinna_payload_len =
			buffer[payload_offset + 4] |
			(buffer[payload_offset + 5] << 8) |
			(buffer[payload_offset + 6] << 16) |
			(buffer[payload_offset + 7] << 24);

	if (len < payload_offset + 10 + aceinna_payload_len) {
		return false;
	}

	uint16_t received_crc =
			(buffer[payload_offset + 8 + aceinna_payload_len] << 8) | // MSB
			buffer[payload_offset + 9 + aceinna_payload_len]; // LSB

	uint16_t calculated_crc = Tool::INS401::CalcCRC(
		&buffer[payload_offset + 2],
		6 + aceinna_payload_len // Message ID(2) + Length(4) + Payload
	);

	if (received_crc != calculated_crc) {
		std::cerr << "CRC mismatch! Received: 0x" << std::hex << received_crc
				<< " Calculated: 0x" << calculated_crc << std::dec << std::endl;
		return false;
	}

	DeviceInfo info;
	info.interface_name =
			info.mac_address = device_mac;

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

	discovered_devices[device_mac] = info;

	return true;
}


void INSDeviceDiscovery::ListenforResponses(int timeout_ms) {
	uint8_t buffer[2048];
	const auto start_time = std::chrono::steady_clock::now();

	while (discovery_running) {
		auto current_time = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
				(current_time - start_time).count();

		if (elapsed > timeout_ms) {
			break;
		}

		ssize_t len = recv(raw_socket_, buffer, sizeof(buffer), 0);
		if (len > 0) {
			ParseResponse(buffer, len);
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			std::cerr << "Receive error: " << strerror(errno) << std::endl;
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	discovery_running = false;
}


bool INSDeviceDiscovery::SendBroadcastPing(const std::string &interface, const std::string &mac_str) {
	// Parsing MAC address
	uint8_t src_mac[6];
	Tool::Ethernet::ParseMACAddressToUint8(mac_str, src_mac);

	// Build ping packet
	const auto packet = BuildPingPacket(src_mac);

	// Set up sockaddr_ll
	sockaddr_ll sll{};
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_halen = ETH_ALEN;
	memcpy(sll.sll_addr, BROADCAST_MAC_, ETH_ALEN);

	ifreq ifr{};
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);
	if (ioctl(raw_socket_, SIOCGIFINDEX, &ifr) < 0) {
		std::cerr << "Failed to get interface index" << std::endl;
		return false;
	}
	sll.sll_ifindex = ifr.ifr_ifindex;

	if (const ssize_t sent = sendto(raw_socket_, packet.data(), packet.size(), 0,
	                                reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll)); sent < 0) {
		std::cerr << "Send failed: " << strerror(errno) << std::endl;
		return false;
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	return true;
}


void INSDeviceDiscovery::DiscoverDevices(int discovery_time_ms) {
	if (geteuid() != 0) {
		std::cerr << "Warning: This program requires root privileges." << std::endl;
		std::cerr << "Please run with sudo." << std::endl;
		return;
	}

	const auto interfaces = Tool::Ethernet::GetNetworkInterfaces();

	if (interfaces.empty()) {
		std::cerr << "No active network interfaces found." << std::endl;
		return;
	}

	for (const auto &iface: interfaces) {
		if (!CreateRawSocket(iface.first)) {
			std::cerr << "Failed to initialize socket on " << iface.first << std::endl;
			continue;
		}

		discovery_running = true;

		std::thread listener(&INSDeviceDiscovery::ListenforResponses, this, discovery_time_ms);

		if (SendBroadcastPing(iface.first, iface.second)) {
			listener.join();
		} else {
			discovery_running = false;
			listener.join();
		}

		close(raw_socket_);
		raw_socket_ = -1;
	}

	if (discovered_devices.empty()) {
		std::cout << "\nNo devices found." << std::endl;
		std::cout << "\nPossible reasons:" << std::endl;
		std::cout << "  • No IMU devices on the network" << std::endl;
		std::cout << "  • Devices are not in discovery mode" << std::endl;
		std::cout << "  • Firewall blocking broadcast packets" << std::endl;
		std::cout << "  • Devices on different network segment" << std::endl;
	}
}
