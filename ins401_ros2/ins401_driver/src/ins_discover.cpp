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
#include <unistd.h>

#include "tool.h"

INSDeviceDiscover::INSDeviceDiscover() : raw_socket_(-1), running_(false) {
  Tool::Ethernet::ParseMACAddressToUint8(BROADCAST_MAC, BROADCAST_MAC_);
  Tool::Ethernet::ConvertUint16ToUint8(COMMAND_START, COMMAND_START_, LSB);
  Tool::Ethernet::ConvertUint16ToUint8(REQUEST_INFO_COMMAND,
                                       REQUEST_INFO_COMMAND_, LSB);
}

INSDeviceDiscover::~INSDeviceDiscover() {
  if (raw_socket_ >= 0) {
    close(raw_socket_);
  }
}

std::map<std::string, DeviceInfo> INSDeviceDiscover::GetDiscoveredDevices() {
  DiscoverDevices();
  return discovered_devices;
}

void INSDeviceDiscover::ClearDiscoveredDevices() { discovered_devices.clear(); }

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
// [Padding: 00 00 ... ]                 // 36 bytes (Fill to 46 with zero
// bytes) [Frame CRC: xx xx xx xx]              // 4 bytes
std::vector<uint8_t>
INSDeviceDiscover::BuildPingPacket(const uint8_t *src_mac) {
  std::vector<uint8_t> frame;
  frame.insert(frame.end(), BROADCAST_MAC_.data(), BROADCAST_MAC_.data() + 6);
  frame.insert(frame.end(), src_mac, src_mac + 6);

  std::vector<uint8_t> aceinna_packet;
  aceinna_packet.insert(aceinna_packet.end(), COMMAND_START_.data(),
                        COMMAND_START_.data() + 2);
  aceinna_packet.insert(aceinna_packet.end(), REQUEST_INFO_COMMAND_.data(),
                        REQUEST_INFO_COMMAND_.data() + 2);

  std::vector<uint8_t> ping_payload;
  aceinna_packet.push_back(0x00);
  aceinna_packet.push_back(0x00);
  aceinna_packet.push_back(0x00);
  aceinna_packet.push_back(0x00);

  // CRC16 - MSB first
  const uint16_t crc16 = Tool::CRC::CalculateINS401_CRC16(
      &aceinna_packet[2], aceinna_packet.size() - 2);
  aceinna_packet.push_back(static_cast<uint8_t>((crc16 >> 8) & 0xFF)); // MSB
  aceinna_packet.push_back(static_cast<uint8_t>(crc16 & 0xFF));        // LSB

  auto eth_payload_length = static_cast<uint16_t>(aceinna_packet.size());
  frame.push_back(static_cast<uint8_t>(eth_payload_length & 0xFF));
  frame.push_back(static_cast<uint8_t>((eth_payload_length >> 8) & 0xFF));

  frame.insert(frame.end(), aceinna_packet.begin(), aceinna_packet.end());
  while (frame.size() - 14 < 46) {
    frame.push_back(0x00);
  }
  return frame;
}

bool INSDeviceDiscover::ParseResponse(const std::string &interface,
                                      const uint8_t *buffer, size_t len) {
  if (len < 60) {
    return false;
  }
  const std::string device_mac = Tool::Ethernet::FormatMacAddress(buffer + 6);
  if (std::memcmp(buffer, BROADCAST_MAC_.data(), 6) == 0) {
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
      buffer[payload_offset + 4] | (buffer[payload_offset + 5] << 8) |
      (buffer[payload_offset + 6] << 16) | (buffer[payload_offset + 7] << 24);
  if (len < payload_offset + 10 + aceinna_payload_len) {
    return false;
  }
  uint16_t received_crc =
      (buffer[payload_offset + 8 + aceinna_payload_len] << 8) | // MSB
      buffer[payload_offset + 9 + aceinna_payload_len];         // LSB
  uint16_t calculated_crc = Tool::CRC::CalculateINS401_CRC16(
      &buffer[payload_offset + 2],
      6 + aceinna_payload_len // Message ID(2) + Length(4) + Payload
  );
  if (received_crc != calculated_crc) {
    std::cerr << "CRC mismatch! Received: 0x" << std::hex << received_crc
              << " Calculated: 0x" << calculated_crc << std::dec << std::endl;
    return false;
  }
  DeviceInfo info;
  info.interface_name = interface; // Filled in DiscoverDevices()
  info.mac_address = device_mac;
  if (aceinna_payload_len > 0) {
    std::string device_data((char *)(buffer + payload_offset + 8),
                            aceinna_payload_len);
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
      info.imu_firmware_version =
          tokens[12] + " " + tokens[13] + " " + tokens[14];
      info.gnss_chip_firmware_version =
          tokens[15] + " " + tokens[16] + " " + tokens[17];
    }
  }
  discovered_devices[device_mac] = info;
  return true;
}

void INSDeviceDiscover::ListenResponses(const std::string &interface,
                                        int timeout_ms) {
  int epfd = -1;
  if (!Tool::Ethernet::SetupEpollForFd(raw_socket_, epfd, EPOLLIN)) {
    return;
  }
  Tool::Ethernet::EpollGuard epoll_guard(epfd);

  constexpr int MAX_EVENTS = 4;
  constexpr size_t BUFFER_SIZE = 2048;
  epoll_event events[MAX_EVENTS];
  std::vector<uint8_t> buffer(BUFFER_SIZE);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (running_.load()) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    size_t wait_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    const int nfds = ::epoll_wait(epfd, events, MAX_EVENTS, wait_ms);
    if (nfds < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "Error: epoll_wait failed: " << strerror(errno) << std::endl;
      break;
    }
    for (int i = 0; i < nfds; ++i) {
      if (events[i].data.fd != raw_socket_) {
        continue;
      }
      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        std::cerr << "Socket error on " << interface << std::endl;
        continue;
      }
      if (events[i].events & EPOLLIN) {
        ssize_t bytes_read;
        do {
          bytes_read =
              ::recv(raw_socket_, buffer.data(), buffer.size(), MSG_DONTWAIT);
          if (bytes_read > 0) {
            ParseResponse(interface, buffer.data(), bytes_read);
          }
        } while (bytes_read > 0 || (bytes_read < 0 && errno == EINTR));
      }
    }
  }
}

void INSDeviceDiscover::DiscoverDevices(int discovery_time_ms) {
  if (geteuid() != 0) {
    std::cerr << "Warning: This program requires root privileges." << std::endl;
    std::cerr << "         Please run with sudo." << std::endl;
    return;
  }
  const auto interfaces = Tool::Ethernet::GetNetworkInterfaces();
  if (interfaces.empty()) {
    std::cerr << "No active network interfaces found." << std::endl;
    return;
  }
  for (const auto &[fst, snd] : interfaces) {
    if (!Tool::Ethernet::CreateAsyncRawSocket(raw_socket_, fst)) {
      std::cerr << "Failed to initialize socket on " << fst << std::endl;
      continue;
    }
    running_ = true;
    std::thread listener(&INSDeviceDiscover::ListenResponses, this, fst,
                         discovery_time_ms);
    std::array<uint8_t, 6> src_mac{};
    Tool::Ethernet::ParseMACAddressToUint8(snd, src_mac);
    std::vector<uint8_t> ping_packet = BuildPingPacket(src_mac.data());
    if (Tool::Ethernet::SendBroadcastPacket(fst, BROADCAST_MAC, snd,
                                            raw_socket_, ping_packet)) {
      listener.join();
    } else {
      running_ = false;
      listener.join();
    }
    close(raw_socket_);
    raw_socket_ = -1;
  }
  if (discovered_devices.empty()) {
    std::cout << "\nNo devices found." << std::endl;
    std::cout << "\nPossible reasons:" << std::endl;
    std::cout << "    • No IMU devices on the network" << std::endl;
    std::cout << "    • Devices are not in discovery mode" << std::endl;
    std::cout << "    • Firewall blocking broadcast packets" << std::endl;
    std::cout << "    • Devices on different network segment" << std::endl;
  }
}
