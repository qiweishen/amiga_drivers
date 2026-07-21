#include "ins401_discover.h"

#include <chrono>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>

#include "ins401_protocol.h"
#include "ins401_tool.h"
#include "logger.h"
#include "utility.h"


namespace INS401 {
namespace {
	constexpr std::string_view kModule = "INS401Discover";
	Common::DriverLog g_log{ std::string(kModule) };
}


INSDeviceDiscover::INSDeviceDiscover() {
	broadcast_mac_ = Ethernet::FormatMACAddress(std::string{ BROADCAST_MAC });
}

INSDeviceDiscover::~INSDeviceDiscover() = default;


std::map<std::string, DeviceInfo> INSDeviceDiscover::DiscoverDevices(int discovery_time_ms) {
	discovered_devices_.clear();

	const auto interfaces = Ethernet::GetNetworkInterfaces();
	if (interfaces.empty()) {
		g_log.info("No active network interfaces found");
		return discovered_devices_;
	}

	running_.store(true);
	std::vector<std::thread> threads;
	threads.reserve(interfaces.size());
	for (const auto &interface: interfaces) {
		threads.emplace_back(
				[this, interface, discovery_time_ms]() { DiscoverOnInterface(interface.first, interface.second, discovery_time_ms); });
	}

	for (auto &t: threads) {
		t.join();
	}
	running_.store(false);

	if (discovered_devices_.empty()) {
		g_log.warn(
				"No devices found. Possible reasons: root privileges required / no IMU devices on the network / not in discovery mode / firewall blocking broadcast / different network segment");
		Common::Log::log_and_throw(kModule, "No INS401 devices found");
	}

	return discovered_devices_;
}


void INSDeviceDiscover::DiscoverOnInterface(const std::string &interface, const std::string &local_mac_str,
											const int discovery_time_ms) {
	try {
		const auto socket_ptr = std::make_shared<EthernetSocket>(interface, broadcast_mac_);
		g_log.trace("Started discovery on interface {} (MAC: {})", interface, local_mac_str);

		SendDiscoveryPing(socket_ptr);

		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(discovery_time_ms);

		while (running_.load() && std::chrono::steady_clock::now() < deadline) {
			const auto response = socket_ptr->Receive(100);
			if (response && !response->empty()) {
				HandleReceive(socket_ptr, response->data(), response->size(), {});
			}
		}
		// Save socket for lifetime management
		sockets_.push_back(socket_ptr);
		--active_interfaces_;
	} catch (const std::exception &e) {
		// Per-interface discovery thread: log and let the other interfaces
		// finish (rethrowing here would std::terminate the process)
		g_log.error("Failed to discover on interface {} - {}", interface, e.what());
	}
}


void INSDeviceDiscover::SendDiscoveryPing(const std::shared_ptr<EthernetSocket> &socket_ptr) const {
	std::vector<uint8_t> ping_packet =
			Ethernet::BuildAceinnaPacket(broadcast_mac_, socket_ptr->GetLocalMac(), REQUEST_INFO_COMMAND_BYTES, nullptr, 0);
	std::ptrdiff_t byte = socket_ptr->Send(ping_packet);
	if (byte < 0) {
		g_log.warn("Failed to send discovery ping on interface {}", socket_ptr->GetInterface());
	} else {
		g_log.trace("Sent discovery ping ({} bytes) on interface {}", byte, socket_ptr->GetInterface());
	}
}


void INSDeviceDiscover::HandleReceive(const std::shared_ptr<EthernetSocket> &socket_ptr, const uint8_t *data, const size_t length,
									  const boost::system::error_code &ec) {
	if (ec) {
		if (ec != boost::asio::error::operation_aborted) {
			g_log.warn("Receive error on interface {} - {}", socket_ptr->GetInterface(), ec.message());
		}
		return;
	}

	if (ParseResponse(socket_ptr->GetInterface(), socket_ptr->GetLocalMac(), data, length)) {
		g_log.trace("Device discovered on {}", socket_ptr->GetInterface());
	}
}


bool INSDeviceDiscover::ParseResponse(const std::string &interface, const MacAddress &local_mac, const uint8_t *buffer,
									  const size_t len) {
	if (len < 60) {
		return false;
	}

	std::string device_mac = Ethernet::ParseMacAddress(buffer + kMacAddressSize);

	// Ignore broadcast packets
	if (std::memcmp(buffer, broadcast_mac_.data(), kMacAddressSize) == 0) {
		return false;
	}

	// Check Aceinna packet header (0x5555)
	if (buffer[kEthernetHeaderSize] != COMMAND_START_BYTES[0] || buffer[kEthernetHeaderSize + 1] != COMMAND_START_BYTES[1]) {
		return false;
	}

	// Check message ID (0x01cc)
	if (buffer[kEthernetHeaderSize + 2] != REQUEST_INFO_COMMAND_BYTES[0] ||
		buffer[kEthernetHeaderSize + 3] != REQUEST_INFO_COMMAND_BYTES[1]) {
		return false;
	}

	// Parse Aceinna payload length (4 bytes)
	uint32_t aceinna_payload_len = buffer[kEthernetHeaderSize + ACEINNA_PRE_AND_ID] |
								   (buffer[kEthernetHeaderSize + ACEINNA_PRE_AND_ID + 1] << 8) |
								   (buffer[kEthernetHeaderSize + ACEINNA_PRE_AND_ID + 2] << 16) |
								   (buffer[kEthernetHeaderSize + ACEINNA_PRE_AND_ID + 3] << 24);

	// CRC validation (2 bytes) - within Aceinna packet
	uint16_t received_crc = (buffer[kEthernetHeaderSize + ACEINNA_HEADER_LEN + aceinna_payload_len]) |
							(buffer[kEthernetHeaderSize + ACEINNA_HEADER_LEN + 1 + aceinna_payload_len] << 8);
	uint16_t calculated_crc = Ethernet::CRC::CalculateINS401_CRC16(
			&buffer[kEthernetHeaderSize + 2], 6 + aceinna_payload_len  // Message ID(2) + Length(4) + Payload
	);
	if (received_crc != calculated_crc) {
		g_log.warn("CRC mismatch! Received: 0x{:X} Calculated: 0x{:X}", received_crc, calculated_crc);
		return false;
	}

	DeviceInfo info;
	info.interface_name = interface;
	info.mac_address = device_mac;
	info.localhost_mac_address = Ethernet::ParseMacAddress(local_mac);

	if (aceinna_payload_len > 0) {
		std::string device_data((char *) (buffer + kEthernetHeaderSize + ACEINNA_HEADER_LEN), aceinna_payload_len);
		// Length(4)
		if (device_data.find(info.product) != std::string::npos) {
			std::istringstream iss(device_data);
			std::vector<std::string> tokens;
			std::string token;
			while (iss >> token) {
				tokens.push_back(token);
			}

			if (tokens.size() >= 18) {
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
	}

	{
		std::scoped_lock lock(devices_mutex_);
		discovered_devices_[device_mac] = info;
	}

	return true;
}
}  // namespace INS401
