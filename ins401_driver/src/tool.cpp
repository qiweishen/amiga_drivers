#include "tool.h"

#include <algorithm>
#include <boost/crc.hpp>
#include <cerrno>
#include <cstring>
#include <fmt/format.h>
#include <ifaddrs.h>
#include <iomanip>
#include <iostream>
#include <net/if.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <netpacket/packet.h>
#include <sys/ioctl.h>



namespace Tool {
	namespace Ethernet {
		std::vector<std::pair<std::string, std::string> > GetNetworkInterfaces() {
			std::vector<std::pair<std::string, std::string> > interfaces;
			ifaddrs *ifaddr;
			if (getifaddrs(&ifaddr) == -1) {
				std::cerr << "Error: Failed to get network interfaces - " << strerror(errno) << std::endl;
				return interfaces;
			}
			for (ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
				if (ifa->ifa_addr == nullptr) {
					continue;
				}
				if (strcmp(ifa->ifa_name, "lo") == 0 || strcmp(ifa->ifa_name, "lo0") == 0) {
					continue;
				}

				if (ifa->ifa_addr->sa_family == AF_PACKET) {
					const auto *s = reinterpret_cast<struct sockaddr_ll *>(ifa->ifa_addr);
					if (s->sll_halen == 6) {
						const int fd = socket(AF_INET, SOCK_DGRAM, 0);
						if (fd < 0)
							continue;

						ifreq ifr{};
						strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);

						bool is_up = false;
						if (ioctl(fd, SIOCGIFFLAGS, &ifr) >= 0) {
							is_up = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
						}
						close(fd);

						if (is_up) {
							std::string mac_str =
									fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", s->sll_addr[0], s->sll_addr[1],
												s->sll_addr[2], s->sll_addr[3], s->sll_addr[4], s->sll_addr[5]);
							interfaces.emplace_back(ifa->ifa_name, mac_str);
						}
					}
				}
			}
			freeifaddrs(ifaddr);
			return interfaces;
		}


		std::string FormatMacAddress(const std::array<uint8_t, 6> &mac_uint8) {
			std::stringstream ss;
			for (size_t i = 0; i < mac_uint8.size(); i++) {
				if (i > 0)
					ss << ":";
				ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(mac_uint8[i]);
			}
			return ss.str();
		}

		std::string FormatMacAddress(const uint8_t *mac_ptr) {
			if (!mac_ptr) {
				return "";
			}
			std::array<uint8_t, 6> mac_uint8{};
			std::copy_n(mac_ptr, 6, mac_uint8.begin());
			return FormatMacAddress(mac_uint8);
		}


		void ParseMACAddressToUint8(const std::string &mac_str, std::array<uint8_t, 6> &mac_uint8) {
			if (mac_str.empty()) {
				throw std::invalid_argument("Empty MAC address string");
			}
			int result = std::sscanf(mac_str.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac_uint8[0], &mac_uint8[1], &mac_uint8[2],
									 &mac_uint8[3], &mac_uint8[4], &mac_uint8[5]);
			if (result != 6) {
				throw std::invalid_argument("Invalid MAC address format: " + mac_str);
			}
		}


		// 		bool CreateAsyncRawSocket(int &raw_socket, const std::string &interface, const std::array<uint8_t, 6> &target_mac,
		// 								  const std::array<uint8_t, 6> &local_mac, size_t buffer_size) {
		// 			// Ensure cleanup of previous socket
		// 			if (raw_socket >= 0) {
		// 				close(raw_socket);
		// 				raw_socket = -1;
		// 			}
		// 			// Create raw socket
		// 			raw_socket = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
		// 			if (raw_socket < 0) {
		// 				std::cerr << "Error: Failed to create raw socket - " << strerror(errno) << std::endl;
		// 				if (errno == EPERM) {
		// 					std::cerr << "Note: This program requires root privileges." << std::endl;
		// 					std::cerr << "      Please run with sudo or use 'Start.bash' to execute program." << std::endl;
		// 				}
		// 				return false;
		// 			}
		//
		// 			// RAII guard for automatic cleanup on failure
		// 			struct SocketGuard {
		// 				int &fd;
		// 				bool released = false;
		// 				explicit SocketGuard(int &socket_fd) : fd(socket_fd) {}
		// 				~SocketGuard() {
		// 					if (!released && fd >= 0) {
		// 						close(fd);
		// 						fd = -1;
		// 					}
		// 				}
		// 				void release() { released = true; }
		// 			} guard(raw_socket);
		//
		// 			// Set non-blocking mode
		// 			int flags = fcntl(raw_socket, F_GETFL, 0);
		// 			if (flags < 0 || fcntl(raw_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
		// 				std::cerr << "Error: Failed to set non-blocking mode - " << strerror(errno) << std::endl;
		// 				return false;
		// 			}
		//
		// 			// Get network interface index
		// 			ifreq ifr{};
		// 			strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);
		// 			ifr.ifr_name[IFNAMSIZ - 1] = '\0';	// Ensure null termination
		// 			if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
		// 				std::cerr << "Error: Failed to get interface index for " << interface << " - " << strerror(errno) << std::endl;
		// 				return false;
		// 			}
		//
		// 			// Bind to specified network interface
		// 			sockaddr_ll sll{};
		// 			sll.sll_family = AF_PACKET;
		// 			sll.sll_ifindex = ifr.ifr_ifindex;
		// 			sll.sll_protocol = htons(ETH_P_ALL);
		//
		// 			if (bind(raw_socket, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0) {
		// 				std::cerr << "Error: Failed to bind to interface " << interface << " - " << strerror(errno) << std::endl;
		// 				return false;
		// 			}
		//
		// 			if (!SetupMACFilter(raw_socket, target_mac, local_mac)) {
		// 				std::cerr << "Warning: MAC filter setup failed" << std::endl;
		// 			}
		//
		// 			// Performance optimization: Set receive buffer size
		// 			if (buffer_size > 0) {
		// 				if (setsockopt(raw_socket, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		// 					std::cerr << "Warning: Failed to set receive buffer size - " << strerror(errno) << std::endl;
		// 				}
		// 			}
		//
		// 			guard.release();
		// 			return true;
		// 		}
		//
		// 		bool CreateAsyncRawSocket(int &raw_socket, const std::string &interface, size_t buffer_size) {
		// 			// Ensure cleanup of previous socket
		// 			if (raw_socket >= 0) {
		// 				close(raw_socket);
		// 				raw_socket = -1;
		// 			}
		// 			// Create raw socket
		// 			raw_socket = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
		// 			if (raw_socket < 0) {
		// 				std::cerr << "Error: Failed to create raw socket - " << strerror(errno) << std::endl;
		// 				if (errno == EPERM) {
		// 					std::cerr << "Note: This program requires root privileges (sudo)" << std::endl;
		// 				}
		// 				return false;
		// 			}
		//
		// 			// RAII guard for automatic cleanup on failure
		// 			struct SocketGuard {
		// 				int &fd;
		// 				bool released = false;
		// 				explicit SocketGuard(int &socket_fd) : fd(socket_fd) {}
		// 				~SocketGuard() {
		// 					if (!released && fd >= 0) {
		// 						close(fd);
		// 						fd = -1;
		// 					}
		// 				}
		// 				void release() { released = true; }
		// 			} guard(raw_socket);
		//
		// 			// Set non-blocking mode
		// 			int flags = fcntl(raw_socket, F_GETFL, 0);
		// 			if (flags < 0 || fcntl(raw_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
		// 				std::cerr << "Error: Failed to set non-blocking mode - " << strerror(errno) << std::endl;
		// 				return false;
		// 			}
		//
		// 			// Get network interface index
		// 			ifreq ifr{};
		// 			strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);
		// 			ifr.ifr_name[IFNAMSIZ - 1] = '\0';	// Ensure null termination
		// 			if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
		// 				std::cerr << "Error: Failed to get interface index for " << interface << " - " << strerror(errno) << std::endl;
		// 				return false;
		// 			}
		//
		// 			// Bind to specified network interface
		// 			sockaddr_ll sll{};
		// 			sll.sll_family = AF_PACKET;
		// 			sll.sll_ifindex = ifr.ifr_ifindex;
		// 			sll.sll_protocol = htons(ETH_P_ALL);
		//
		// 			if (bind(raw_socket, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0) {
		// 				std::cerr << "Error: Failed to bind to interface " << interface << " - " << strerror(errno) << std::endl;
		// 				return false;
		// 			}
		//
		// 			// Performance optimization: Set receive buffer size
		// 			if (buffer_size > 0) {
		// 				if (setsockopt(raw_socket, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		// 					std::cerr << "Warning: Failed to set receive buffer size - " << strerror(errno) << std::endl;
		// 				}
		// 			}
		//
		// 			struct fanout_args {
		// 				uint16_t id;
		// 				uint16_t type;
		// 			} fanout = { 0, PACKET_FANOUT_HASH };
		//
		// 			if (setsockopt(raw_socket, SOL_PACKET, PACKET_FANOUT, &fanout, sizeof(fanout)) < 0) {
		// 				std::cerr << "Warning: Failed to set packet fanout - " << strerror(errno) << std::endl;
		// 			}
		//
		// 			guard.release();
		// 			return true;
		// 		}
		//
		//
		// 		bool SetupMACFilter(int raw_socket, const std::array<uint8_t, 6> &target_mac, const std::array<uint8_t, 6> &local_mac)
		// {
		// 			/* BPF bytecode filter program
		// 			 * The filter checks Ethernet frames for MAC address matching
		// 			 * Ethernet frame structure:
		// 			 * Offset 0-5:   Destination MAC
		// 			 * Offset 6-11:  Source MAC
		// 			 */
		// 			// Prepare MAC addresses for BPF comparison
		// 			uint32_t dev_mac_first_4, local_mac_first_4;
		// 			uint16_t dev_mac_last_2, local_mac_last_2;
		//
		// 			std::memcpy(&dev_mac_first_4, target_mac.data(), 4);
		// 			std::memcpy(&dev_mac_last_2, target_mac.data() + 4, 2);
		// 			std::memcpy(&local_mac_first_4, local_mac.data(), 4);
		// 			std::memcpy(&local_mac_last_2, local_mac.data() + 4, 2);
		//
		// #if __BYTE_ORDER == __LITTLE_ENDIAN
		// 			dev_mac_first_4 = htonl(dev_mac_first_4);
		// 			dev_mac_last_2 = htons(dev_mac_last_2);
		// 			local_mac_first_4 = htonl(local_mac_first_4);
		// 			local_mac_last_2 = htons(local_mac_last_2);
		// #endif
		//
		// 			sock_filter filter[] = {
		// 				/* === Check: Source=Device AND Destination=Local (incoming) === */
		// 				// [0] Check source MAC = device_mac (first 4 bytes)
		// 				BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
		// 				// [1] Jump if not equal (skip to second check at [8])
		// 				BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, dev_mac_first_4, 0, 6),
		// 				// [2] Check source MAC = device_mac (last 2 bytes)
		// 				BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 10),
		// 				// [3] Jump if not equal (skip to second check at [8])
		// 				BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, dev_mac_last_2, 0, 4),
		// 				// [4] Check destination MAC = local_mac (first 4 bytes)
		// 				BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		// 				// [5] Jump if not equal (skip to second check at [8])
		// 				BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_first_4, 0, 2),
		// 				// [6] Check destination MAC = local_mac (last 2 bytes)
		// 				BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
		// 				// [7] Jump to ACCEPT [17] if equal, else continue to [8]
		// 				BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_last_2, 9, 0),
		//
		// 				/* === Check: Source=Local AND Destination=Device (outgoing) === */
		// 				// [8] Check source MAC = local_mac (first 4 bytes)
		// 				BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
		// 				// [9] Jump if not equal (skip to DROP at [16])
		// 				BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_first_4, 0, 6),
		// 				// [10] Check source MAC = local_mac (last 2 bytes)
		// 				BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 10),
		// 				// [11] Jump if not equal (skip to DROP at [16])
		// 				BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_last_2, 0, 4),
		// 				// [12] Check destination MAC = device_mac (first 4 bytes)
		// 				BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		// 				// [13] Jump if not equal (skip to DROP at [16])
		// 				BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, dev_mac_first_4, 0, 2),
		// 				// [14] Check destination MAC = device_mac (last 2 bytes)
		// 				BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
		// 				// [15] Jump to ACCEPT [17] if equal, else continue to [16]
		// 				BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, dev_mac_last_2, 1, 0),
		//
		// 				// [16] No match - drop packet
		// 				BPF_STMT(BPF_RET | BPF_K, 0),
		// 				// [17] Both conditions matched - accept packet
		// 				BPF_STMT(BPF_RET | BPF_K, 65535),
		// 			};
		//
		// 			sock_fprog fprog = {
		// 				.len = sizeof(filter) / sizeof(filter[0]),
		// 				.filter = filter,
		// 			};
		//
		// 			return setsockopt(raw_socket, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof(fprog)) >= 0;
		// 		}


		/**
		 * Ethernet Frame Structure:
		 *
		 * +-------------------------------------+
		 * | Ethernet Header (14 bytes)         |
		 * +-------------------------------------+
		 * | Destination MAC     | 6 bytes      |
		 * | Source MAC          | 6 bytes      |
		 * | Length/Type         | 2 bytes      | (LSB-first, for input message)
		 * +-------------------------------------+
		 * | Ethernet Payload (46-1500 bytes)   |
		 * +-------------------------------------+
		 * | Aceinna Packet:                    |
		 * |   Header            | 2 bytes      | (0x5555 or similar)
		 * |   Message ID        | 2 bytes      | (LSB-first)
		 * |   Payload Length    | 4 bytes      | (LSB-first)
		 * |   Payload Data      | N bytes      | (variable length)
		 * |   CRC16 Checksum    | 2 bytes      | (MSB-first)
		 * +-------------------------------------+
		 * | Padding             | M bytes      | (0x00 padding to reach 46 bytes minimum)
		 * +-------------------------------------+
		 * | Frame CRC (FCS)     | 4 bytes      | (added by hardware)
		 * +-------------------------------------+
		 */
		std::vector<uint8_t> BuildPacket(const std::array<uint8_t, 2> &command_start, const std::array<uint8_t, 2> &message_id,
										 const uint8_t *payload, size_t payload_length, const std::array<uint8_t, 6> &target_mac,
										 const std::array<uint8_t, 6> &local_mac) {
			std::vector<uint8_t> frame;
			frame.insert(frame.end(), target_mac.data(), target_mac.data() + 6);
			frame.insert(frame.end(), local_mac.data(), local_mac.data() + 6);

			std::vector<uint8_t> aceinna_packet;
			aceinna_packet.insert(aceinna_packet.end(), command_start.data(), command_start.data() + 2);
			aceinna_packet.insert(aceinna_packet.end(), message_id.data(), message_id.data() + 2);

			if (payload != nullptr && payload_length > 0) {
				// Payload length field - LSB-first
				const auto length = static_cast<uint32_t>(payload_length);
				aceinna_packet.push_back(static_cast<uint8_t>(length & 0xFF));
				aceinna_packet.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
				aceinna_packet.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
				aceinna_packet.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
				// Payload field
				aceinna_packet.insert(aceinna_packet.end(), payload, payload + payload_length);
			} else {
				aceinna_packet.push_back(0x00);
				aceinna_packet.push_back(0x00);
				aceinna_packet.push_back(0x00);
				aceinna_packet.push_back(0x00);
			}

			// CRC16 - LSB-first - From Message ID to Payload
			const uint16_t crc16 = CRC::CalculateINS401_CRC16(&aceinna_packet[2], aceinna_packet.size() - 2);
			aceinna_packet.push_back(static_cast<uint8_t>(crc16 & 0xFF));
			aceinna_packet.push_back(static_cast<uint8_t>((crc16 >> 8) & 0xFF));

			// ETH length field - LSB-first
			auto eth_payload_length = static_cast<uint16_t>(aceinna_packet.size());
			frame.push_back(static_cast<uint8_t>(eth_payload_length & 0xFF));
			frame.push_back(static_cast<uint8_t>((eth_payload_length >> 8) & 0xFF));

			frame.insert(frame.end(), aceinna_packet.begin(), aceinna_packet.end());

			// Padding to reach minimum Ethernet frame size (46 bytes payload)
			if (aceinna_packet.size() < 46) {
				size_t padding_size = 46 - aceinna_packet.size();
				frame.insert(frame.end(), padding_size, 0x00);
			}
			return frame;
		}


		// bool SendBroadcastPacket(const std::string &interface, const std::array<uint8_t, 6> &target_mac, const int &raw_socket,
		// 						 const std::vector<uint8_t> &packet) {
		// 	// Set up sockaddr_ll
		// 	sockaddr_ll sll{};
		// 	std::memset(&sll, 0, sizeof(sll));
		// 	sll.sll_family = AF_PACKET;
		// 	sll.sll_protocol = htons(ETH_P_ALL);
		// 	sll.sll_halen = ETH_ALEN;
		// 	std::memcpy(sll.sll_addr, target_mac.data(), ETH_ALEN);
		// 	ifreq ifr{};
		// 	std::memset(&ifr, 0, sizeof(ifr));
		// 	std::strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);
		// 	if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
		// 		std::cerr << "Failed to get interface index" << std::endl;
		// 		return false;
		// 	}
		// 	sll.sll_ifindex = ifr.ifr_ifindex;
		// 	if (const ssize_t sent =
		// 				sendto(raw_socket, packet.data(), packet.size(), 0, reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll));
		// 		sent < 0) {
		// 		std::cerr << "Send failed: " << strerror(errno) << std::endl;
		// 		return false;
		// 	}
		// 	return true;
		// }
		//
		//
		// bool SetupEpollForFd(int sock_fd, int &epfd_out, const uint32_t events) {
		// 	epfd_out = epoll_create1(0);
		// 	if (epfd_out < 0) {
		// 		std::cerr << "Error: epoll_create1 failed: " << strerror(errno) << std::endl;
		// 		return false;
		// 	}
		// 	epoll_event ev{};
		// 	ev.events = events;
		// 	ev.data.fd = sock_fd;
		// 	if (epoll_ctl(epfd_out, EPOLL_CTL_ADD, sock_fd, &ev) < 0) {
		// 		std::cerr << "Error: epoll_ctl ADD failed for fd=" << sock_fd << " : " << strerror(errno) << std::endl;
		// 		close(epfd_out);
		// 		epfd_out = -1;
		// 		return false;
		// 	}
		// 	return true;
		// }
	}  // namespace Ethernet


	namespace CRC {
		uint16_t CalculateINS401_CRC16(const uint8_t *buf, const uint16_t &length) {
			uint16_t crc = 0x1D0F;
			for (int i = 0; i < length; i++) {
				crc ^= buf[i] << 8;
				for (int j = 0; j < 8; j++) {
					if (crc & 0x8000) {
						crc = (crc << 1) ^ 0x1021;
					} else {
						crc = crc << 1;
					}
				}
			}
			return ((crc << 8) & 0xFF00) | ((crc >> 8) & 0xFF);
		}


		uint32_t CalculateRTCM3_CRC24(const void *data, std::size_t nBytes) {
			// Bits = 24
			// TruncPoly = 0x1864CFB
			// InitRem = 0
			// FinalXor = 0
			// ReflectIn = false
			// ReflectRem = false
			using crc24_t = boost::crc_optimal<24, 0x1864CFB, 0, 0, false, false>;
			crc24_t crc;
			crc.process_bytes(data, nBytes);
			const uint32_t result = crc.checksum();
			return result;
		}
	}  // namespace CRC


	namespace Utility {
		std::vector<std::string> SplitString(const std::string &str, char delimiter) {
			std::vector<std::string> tokens;
			std::stringstream ss(str);
			std::string token;
			while (std::getline(ss, token, delimiter)) {
				tokens.push_back(token);
			}
			return tokens;
		}
	}  // namespace Utility

}  // namespace Tool
