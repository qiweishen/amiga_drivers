#include "ins_discover.h"

#include <chrono>
#include <iostream>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>

#include "tool.h"



INSDeviceDiscover::INSDeviceDiscover() : work_guard_(boost::asio::make_work_guard(io_context_)) {
	Tool::Ethernet::ParseMACAddressToUint8(BROADCAST_MAC, broadcast_mac_);
}


INSDeviceDiscover::~INSDeviceDiscover() {
	Stop();
}


std::map<std::string, DeviceInfo> INSDeviceDiscover::DiscoverDevices(int discovery_time_ms) {
	discovered_devices_.clear();

	// Get all network interfaces
	const auto interfaces = Tool::Ethernet::GetNetworkInterfaces();
	if (interfaces.empty()) {
		spdlog::info("No active network interfaces found");
		return discovered_devices_;
	}
	running_ = true;
	active_interfaces_ = 0;

	// Start discovery on each interface
	for (const auto &[interface, mac]: interfaces) {
		std::thread([this, interface, mac, discovery_time_ms]() { DiscoverOnInterface(interface, mac, discovery_time_ms); }).detach();
		++active_interfaces_;
	}

	// Run IO context until all interfaces complete
	std::thread io_thread([this]() { io_context_.run(); });

	// Wait for all interfaces to complete or timeout
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(discovery_time_ms + 100);
	while (active_interfaces_ > 0 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	// Stop IO context
	Stop();
	if (io_thread.joinable()) {
		io_thread.join();
	}

	if (discovered_devices_.empty()) {
		std::cout << "\nNo devices found." << std::endl;
		std::cout << "\nPossible reasons:" << std::endl;
		std::cout << "    • No IMU devices on the network" << std::endl;
		std::cout << "    • Devices are not in discovery mode" << std::endl;
		std::cout << "    • Firewall blocking broadcast packets" << std::endl;
		std::cout << "    • Devices on different network segment" << std::endl;
	}

	return discovered_devices_;
}


void INSDeviceDiscover::Stop() {
	running_ = false;
	work_guard_.reset();

	// Cancel all timers
	for (auto &timer: timers_) {
		timer.cancel();
	}

	// Close all sockets
	for (auto &socket: sockets_) {
		if (socket && socket->isRunning()) {
			socket->Close();
		}
	}

	// Stop IO context
	io_context_.stop();
}


void INSDeviceDiscover::DiscoverOnInterface(const std::string &interface, const std::string &local_mac, int discovery_time_ms) {
	try {
		// Create async socket
		AsyncPacketSocket::Config socket_config;
		socket_config.promiscuous = true;
		socket_config.interface_name = interface;
		auto socket = std::make_shared<AsyncPacketSocket>(io_context_, socket_config);

		// Open interface
		if (!socket->Open()) {
			--active_interfaces_;
			return;
		}
		spdlog::info("Started discovery on interface {} (MAC: {})", interface, local_mac);

		// Convert local MAC address
		std::array<uint8_t, 6> src_mac{};
		Tool::Ethernet::ParseMACAddressToUint8(local_mac, src_mac);

		// Send discovery packet
		SendDiscoveryPing(*socket, interface, src_mac);

		// Setup timeout timer
		timers_.emplace_back(io_context_);
		auto &timer = timers_.back();
		timer.expires_from_now(boost::posix_time::milliseconds(discovery_time_ms));
		timer.async_wait([this, socket](const boost::system::error_code &ec) {
			if (!ec) {
				socket->Close();
				--active_interfaces_;
			}
		});

		using ReceiveHandler = AsyncPacketSocket::ReceiveHandler;
		auto receive_handler = std::make_shared<ReceiveHandler>();

		// Start async receive std::function<void(const uint8_t *, size_t, const boost::system::error_code &)>
		*receive_handler = [this, interface, local_mac, socket, receive_handler](const uint8_t *data, size_t length,
																				 const boost::system::error_code &ec) {
			if (!ec && running_) {
				HandleReceive(interface, local_mac, data, length, ec);
				socket->AsyncReceive(*receive_handler);
			} else if (ec != boost::asio::error::operation_aborted) {
				spdlog::error("Receive error on {}: {}", interface, ec.message());
			}
		};
		socket->AsyncReceive(*receive_handler);

		// Save socket for lifetime management
		sockets_.push_back(socket);

	} catch (const std::exception &e) {
		spdlog::error("Exception on interface {}: {}", interface, e.what());
		--active_interfaces_;
	}
}


void INSDeviceDiscover::SendDiscoveryPing(AsyncPacketSocket &socket, const std::string &interface,
										  const std::array<uint8_t, 6> &src_mac) const {
	// Build discovery packet
	const std::vector<uint8_t> ping_packet =
			Tool::Ethernet::BuildPacket(COMMAND_START_BYTES, REQUEST_INFO_COMMAND_BYTES, nullptr, 0, broadcast_mac_, src_mac);

	// Send asynchronously
	socket.AsyncSend(ping_packet.data(), ping_packet.size(), [interface](const boost::system::error_code &ec, size_t bytes_sent) {
		if (ec) {
			spdlog::error("Failed to send discovery packet on {}: {}", interface, ec.message());
		} else {
			spdlog::trace("Discovery packet sent on {} ({} bytes)", interface, bytes_sent);
		}
	});
}


void INSDeviceDiscover::HandleReceive(const std::string &interface, const std::string &local_mac, const uint8_t *data, size_t length,
									  const boost::system::error_code &ec) {
	if (ec) {
		if (ec != boost::asio::error::operation_aborted) {
			spdlog::error("Receive error: {}", ec.message());
		}
		return;
	}

	// Parse response
	if (ParseResponse(interface, local_mac, data, length)) {
		spdlog::trace("Device discovered on {}", interface);
	}
}


bool INSDeviceDiscover::ParseResponse(const std::string &interface, const std::string &local_mac, const uint8_t *buffer, size_t len) {
	// Basic length check
	if (len < 60) {
		return false;
	}

	// Get source MAC address
	const std::string device_mac = Tool::Ethernet::FormatMacAddress(buffer + 6);

	// Ignore broadcast packets
	if (std::memcmp(buffer, broadcast_mac_.data(), 6) == 0) {
		return false;
	}

	// Parse payload length
	uint16_t payload_length = buffer[12] | (buffer[13] << 8);
	if (payload_length < 10) {
		return false;
	}

	size_t payload_offset = 14;

	// Check Aceinna packet header (0x5555)
	if (len < payload_offset + 2) {
		return false;
	}
	if (buffer[payload_offset] != COMMAND_START_BYTES[0] || buffer[payload_offset + 1] != COMMAND_START_BYTES[1]) {
		return false;
	}

	// Check message ID (0x01cc)
	if (len < payload_offset + 4) {
		return false;
	}
	if (buffer[payload_offset + 2] != REQUEST_INFO_COMMAND_BYTES[0] || buffer[payload_offset + 3] != REQUEST_INFO_COMMAND_BYTES[1]) {
		return false;
	}

	// Parse Aceinna payload length
	if (len < payload_offset + 8) {
		return false;
	}
	uint32_t aceinna_payload_len = buffer[payload_offset + 4] | (buffer[payload_offset + 5] << 8) |
								   (buffer[payload_offset + 6] << 16) | (buffer[payload_offset + 7] << 24);

	if (len < payload_offset + 10 + aceinna_payload_len) {
		return false;
	}

	// CRC validation
	uint16_t received_crc =
			(buffer[payload_offset + 8 + aceinna_payload_len]) | (buffer[payload_offset + 9 + aceinna_payload_len] << 8);
	uint16_t calculated_crc = Tool::CRC::CalculateINS401_CRC16(&buffer[payload_offset + 2],
															   6 + aceinna_payload_len	// Message ID(2) + Length(4) + Payload
	);

	if (received_crc != calculated_crc) {
		spdlog::error("CRC mismatch! Received: 0x{:X} Calculated: 0x{:X}", received_crc, calculated_crc);
		return false;
	}

	// Parse device info
	DeviceInfo info;
	info.interface_name = interface;
	info.mac_address = device_mac;
	info.localhost_mac_address = local_mac;

	if (aceinna_payload_len > 0) {
		std::string device_data((char *) (buffer + payload_offset + 8), aceinna_payload_len);
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

	// Thread-safe save device info
	{
		std::lock_guard<std::mutex> lock(devices_mutex_);
		discovered_devices_[device_mac] = info;
	}

	return true;
}
