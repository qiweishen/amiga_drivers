#include "tool.h"

#include <algorithm>
#include <boost/crc.hpp>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <iomanip>
#include <iostream>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sstream>
#include <sys/ioctl.h>
#include <thread>
#include <vector>



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
				if (ifa->ifa_addr->sa_family == AF_PACKET) {
					if (const auto *s = reinterpret_cast<struct sockaddr_ll *>(ifa->ifa_addr);
						s->sll_halen == 6 && strcmp(ifa->ifa_name, "lo") != 0) {
						const int fd = socket(AF_INET, SOCK_DGRAM, 0);
						ifreq ifr{};
						memset(&ifr, 0, sizeof(ifr));
						strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);

						bool is_up = false;
						if (ioctl(fd, SIOCGIFFLAGS, &ifr) >= 0) {
							is_up = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
						}
						close(fd);
						if (is_up) {
							std::stringstream mac_stream;
							for (int i = 0; i < 6; i++) {
								if (i > 0)
									mac_stream << ":";
								mac_stream << std::hex << std::setw(2) << std::setfill('0') << (int) s->sll_addr[i];
							}
							interfaces.emplace_back(ifa->ifa_name, mac_stream.str());
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


		void ConvertUint16ToUint8(const uint16_t &uint16, std::array<uint8_t, 2> &uint8, ENDIAN_TYPE type) {
			if (type == LSB) {
				uint8[0] = static_cast<uint8_t>(uint16 & 0xFF);
				uint8[1] = static_cast<uint8_t>((uint16 >> 8) & 0xFF);
			} else if (type == MSB) {
				uint8[0] = static_cast<uint8_t>((uint16 >> 8) & 0xFF);
				uint8[1] = static_cast<uint8_t>(uint16 & 0xFF);
			} else {
				throw std::invalid_argument("Invalid ENDIAN_TYPE specified");
			}
		}


		void ParseMACAddressToUint8(const std::string &mac_str, std::array<uint8_t, 6> &mac_uint8) {
			if (mac_str.empty()) {
				throw std::invalid_argument("Empty MAC address string");
			}
			int result = std::sscanf(mac_str.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac_uint8[0], &mac_uint8[1], &mac_uint8[2], &mac_uint8[3],
									 &mac_uint8[4], &mac_uint8[5]);
			if (result != 6) {
				throw std::invalid_argument("Invalid MAC address format: " + mac_str);
			}
		}


		bool CreateAsyncRawSocket(int &raw_socket, const std::string &interface, size_t buffer_size) {
			// Ensure cleanup of previous socket
			if (raw_socket >= 0) {
				close(raw_socket);
				raw_socket = -1;
			}
			// Create raw socket
			raw_socket = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
			if (raw_socket < 0) {
				std::cerr << "Error: Failed to create raw socket - " << strerror(errno) << std::endl;
				if (errno == EPERM) {
					std::cerr << "Note: This program requires root privileges (sudo)" << std::endl;
				}
				return false;
			}

			// RAII guard for automatic cleanup on failure
			struct SocketGuard {
				int &fd;
				bool released = false;
				explicit SocketGuard(int &socket_fd) : fd(socket_fd) {}
				~SocketGuard() {
					if (!released && fd >= 0) {
						close(fd);
						fd = -1;
					}
				}
				void release() { released = true; }
			} guard(raw_socket);

			// Set non-blocking mode
			int flags = fcntl(raw_socket, F_GETFL, 0);
			if (flags < 0 || fcntl(raw_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
				std::cerr << "Error: Failed to set non-blocking mode - " << strerror(errno) << std::endl;
				return false;
			}

			// Get network interface index
			ifreq ifr{};
			strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);
			ifr.ifr_name[IFNAMSIZ - 1] = '\0';	// Ensure null termination
			if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
				std::cerr << "Error: Failed to get interface index for " << interface << " - " << strerror(errno) << std::endl;
				return false;
			}

			// Bind to specified network interface
			sockaddr_ll sll{};
			sll.sll_family = AF_PACKET;
			sll.sll_ifindex = ifr.ifr_ifindex;
			sll.sll_protocol = htons(ETH_P_ALL);

			if (bind(raw_socket, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0) {
				std::cerr << "Error: Failed to bind to interface " << interface << " - " << strerror(errno) << std::endl;
				return false;
			}

			// Performance optimization: Set receive buffer size
			if (buffer_size > 0) {
				if (setsockopt(raw_socket, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
					std::cerr << "Warning: Failed to set receive buffer size - " << strerror(errno) << std::endl;
				}
			}

			struct fanout_args {
				uint16_t id;
				uint16_t type;
			} fanout = { 0, PACKET_FANOUT_HASH };

			if (setsockopt(raw_socket, SOL_PACKET, PACKET_FANOUT, &fanout, sizeof(fanout)) < 0) {
				std::cerr << "Warning: Failed to set packet fanout - " << strerror(errno) << std::endl;
			}

			guard.release();
			return true;
		}


		bool SendBroadcastPacket(const std::string &interface, const std::string &dest_mac_str, const std::string &src_mac_str, const int &raw_socket,
								 const std::vector<uint8_t> &packet) {
			// Parsing MAC address
			std::array<uint8_t, 6> dest_mac{}, src_mac{};
			ParseMACAddressToUint8(dest_mac_str, dest_mac);
			ParseMACAddressToUint8(src_mac_str, src_mac);
			// Set up sockaddr_ll
			sockaddr_ll sll{};
			std::memset(&sll, 0, sizeof(sll));
			sll.sll_family = AF_PACKET;
			sll.sll_protocol = htons(ETH_P_ALL);
			sll.sll_halen = ETH_ALEN;
			std::memcpy(sll.sll_addr, dest_mac.data(), ETH_ALEN);
			ifreq ifr{};
			std::memset(&ifr, 0, sizeof(ifr));
			std::strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);
			if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
				std::cerr << "Failed to get interface index" << std::endl;
				return false;
			}
			sll.sll_ifindex = ifr.ifr_ifindex;
			if (const ssize_t sent = sendto(raw_socket, packet.data(), packet.size(), 0, reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll));
				sent < 0) {
				std::cerr << "Send failed: " << strerror(errno) << std::endl;
				return false;
			}
			return true;
		}


		bool SetupEpollForFd(int sock_fd, int &epfd_out, const uint32_t events) {
			epfd_out = epoll_create1(0);
			if (epfd_out < 0) {
				std::cerr << "Error: epoll_create1 failed: " << strerror(errno) << std::endl;
				return false;
			}
			epoll_event ev{};
			ev.events = events;
			ev.data.fd = sock_fd;
			if (epoll_ctl(epfd_out, EPOLL_CTL_ADD, sock_fd, &ev) < 0) {
				std::cerr << "Error: epoll_ctl ADD failed for fd=" << sock_fd << " : " << strerror(errno) << std::endl;
				close(epfd_out);
				epfd_out = -1;
				return false;
			}
			return true;
		}
	}  // namespace Ethernet


	namespace CRC {
		uint16_t CalculateINS401_CRC16(const uint8_t *buf, const uint16_t &length) {
			uint16_t crc = 0x1D0F;
			for (uint16_t i = 0; i < length; i++) {
				crc ^= static_cast<uint16_t>(buf[i]) << 8;
				for (int j = 0; j < 8; j++) {
					if (crc & 0x8000) {
						crc = static_cast<uint16_t>(((crc << 1) ^ 0x1021) & 0xFFFF);
					} else {
						crc = static_cast<uint16_t>((crc << 1) & 0xFFFF);
					}
				}
			}
			return crc;
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
}  // namespace Tool
