#pragma once

#include <iomanip>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

#include "data_type.h"



namespace Tool {
	namespace Ethernet {
		/**
		 * Get a list of active network interfaces and their MAC addresses
		 * @return Vector of pairs (interface name, MAC address string)
		 */
		std::vector<std::pair<std::string, std::string> > GetNetworkInterfaces();


		/**
		 * Format MAC address to string representation
		 * @param mac_uint8 MAC address as 6-byte array
		 * @return Formatted string like "xx:xx:xx:xx:xx:xx"
		 */
		std::string FormatMacAddress(const std::array<uint8_t, 6> &mac_uint8);
		/**
		 * Format MAC address from raw pointer to string representation
		 * @param mac_ptr Pointer to 6-byte MAC address
		 * @return Formatted string like "xx:xx:xx:xx:xx:xx"
		 */
		std::string FormatMacAddress(const uint8_t *mac_ptr);


		/**
		 * Parse MAC address string to byte array
		 * @param mac_str MAC address string (format: "xx:xx:xx:xx:xx:xx")
		 * @param mac_uint8 Output 6-byte array
		 * @throws std::invalid_argument if format is invalid
		 */
		void ParseMACAddressToUint8(const std::string &mac_str, std::array<uint8_t, 6> &mac_uint8);


		/**
		 * Create an asynchronous raw socket
		 * @param raw_socket Output parameter, returns the created socket descriptor
		 * @param interface Network interface name (e.g., "eth0")
		 * @param target_mac MAC address of the target device, needed for BPF filter
		 * @param local_mac MAC address of the local interface, needed for BPF filter
		 * @param buffer_size Optional receive buffer size (4 * 1024 * 1024 for default)
		 * @return true on success, false on failure
		 */
		bool CreateAsyncRawSocket(int &raw_socket, const std::string &interface, const std::array<uint8_t, 6> &target_mac,
								  const std::array<uint8_t, 6> &local_mac, size_t buffer_size = 4 * 1024 * 1024);
		/**
		 * Create an asynchronous raw socket
		 * @param raw_socket Output parameter, returns the created socket descriptor
		 * @param interface Network interface name (e.g., "eth0")
		 * @param buffer_size Optional receive buffer size (4 * 1024 * 1024 for default)
		 * @return true on success, false on failure
		 */
		bool CreateAsyncRawSocket(int &raw_socket, const std::string &interface, size_t buffer_size = 4 * 1024 * 1024);


		/**
		 * Attach a BPF filter for bidirectional communication with a specific device
		 *
		 * Accepts packets that are:
		 * - FROM target_mac TO local_mac (incoming)
		 * - FROM local_mac TO target_mac (outgoing, for packet capture)
		 *
		 * @param raw_socket The raw socket file descriptor
		 * @param target_mac MAC address of the target device
		 * @param local_mac MAC address of the local interface
		 * @return true if filter was successfully attached
		 */
		bool SetupMACFilter(int raw_socket, const std::array<uint8_t, 6> &target_mac, const std::array<uint8_t, 6> &local_mac);


		/**
		 * Build an Ethernet frame containing payload data in Aceinna packet format
		 * @param payload        Pointer to payload data to be transmitted
		 * @param payload_length Length of payload data in bytes
		 * @param target_mac     Destination MAC address (device MAC)
		 * @param local_mac      Source MAC address (local network interface MAC)
		 *
		 * @return std::vector<uint8_t> Complete Ethernet frame ready for transmission
		 */
		std::vector<uint8_t> BuildPacket(const std::array<uint8_t, 2> &command_start, const std::array<uint8_t, 2> &message_id,
										 const uint8_t *payload, size_t payload_length, const std::array<uint8_t, 6> &target_mac,
										 const std::array<uint8_t, 6> &local_mac);


		/**
		 * Send a broadcast Ethernet packet
		 * @param interface Network interface name
		 * @param target_mac MAC address of the target device
		 * @param raw_socket Raw socket descriptor
		 * @param packet Packet data to send
		 * @return true on success, false on failure
		 */
		bool SendBroadcastPacket(const std::string &interface, const std::array<uint8_t, 6> &target_mac, const int &raw_socket,
								 const std::vector<uint8_t> &packet);


		/**
		 * Set up epoll to monitor a socket file descriptor for events
		 * @param sock_fd    The socket file descriptor to monitor (should already be set to non-blocking)
		 * @param epfd_out   Output parameter: returns the created epoll instance file descriptor
		 * @param events     The epoll event mask (e.g., EPOLLIN or EPOLLIN | EPOLLET, default is EPOLLIN)
		 * @return true on success, false on failure
		 */
		bool SetupEpollForFd(int sock_fd, int &epfd_out, uint32_t events = EPOLLIN);
		class EpollGuard {
			int epfd_;

		public:
			explicit EpollGuard(int fd) : epfd_(fd) {}
			~EpollGuard() {
				if (epfd_ >= 0)
					::close(epfd_);
			}
			[[nodiscard]] int get() const { return epfd_; }
		};
	}  // namespace Ethernet


	namespace CRC {
		uint16_t CalculateINS401_CRC16(const uint8_t *buf, const uint16_t &length);

		uint32_t CalculateRTCM3_CRC24(const void *data, std::size_t nBytes);
	}  // namespace CRC


	namespace Utility {
		std::vector<std::string> SplitString(const std::string &str, char delimiter);
	}

}  // namespace Tool
