/**
 * @file ins_discover.h
 * @brief INS401 device discovery over Ethernet using raw sockets.
 *
 * This module provides functionality to discover INS401 devices on the local
 * network by sending broadcast discovery packets and collecting device
 * information responses.
 *
 * @author Qiwei
 * @date 2025
 *
 */

#pragma once

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ethernet_socket.h"



/**
 * @struct DeviceInfo
 * @brief Holds detailed information about a discovered INS401 device.
 *
 * This structure contains all identifying and version information
 * returned by an INS401 device during the discovery process.
 */
struct DeviceInfo {
	std::string interface_name;				 ///< Network interface where device was discovered
	std::string mac_address;				 ///< Device MAC address
	std::string localhost_mac_address;		 ///< Local host MAC address used for discovery
	std::string product = "INS401";			 ///< Product identifier
	std::string part_number;				 ///< Device part number
	std::string serial_number;				 ///< Device serial number
	std::string hardware_version;			 ///< Hardware revision string
	std::string imu_serial_number;			 ///< IMU module serial number
	std::string firmware_version;			 ///< Main firmware version
	std::string bootloader_version;			 ///< Bootloader version
	std::string imu_firmware_version;		 ///< IMU firmware version
	std::string gnss_chip_firmware_version;	 ///< GNSS chip firmware version
};

/**
 * @class INSDeviceDiscover
 * @brief Discovers INS401 devices on all available network interfaces.
 *
 * This class manages the discovery process for INS401 devices by:
 * - Creating raw Ethernet sockets on each network interface
 * - Sending broadcast discovery ping packets
 * - Collecting and parsing device responses
 * - Aggregating results from all interfaces
 *
 * @note This class requires appropriate permissions to create raw sockets
 *       (typically root/CAP_NET_RAW on Linux).
 *
 * Example usage:
 * @code
 * INSDeviceDiscover discoverer;
 * auto devices = discoverer.DiscoverDevices(1000);  // 1 second timeout
 * for (const auto& [mac, info] : devices) {
 *     std::cout << "Found: " << info.serial_number << " on " << info.interface_name << "\n";
 * }
 * @endcode
 */
class INSDeviceDiscover {
public:
	/**
	 * @brief Constructs the device discoverer.
	 *
	 * Initializes internal state and pre-parses the broadcast MAC address.
	 */
	explicit INSDeviceDiscover();

	/**
	 * @brief Destructor.
	 *
	 * Ensures all discovery operations are stopped and resources are released.
	 */
	~INSDeviceDiscover();

	/**
	 * @brief Discovers INS401 devices on all available network interfaces.
	 *
	 * This method performs a synchronous discovery operation by:
	 * 1. Enumerating all network interfaces
	 * 2. Sending discovery ping packets on each interface
	 * 3. Waiting for and collecting device responses
	 * 4. Aggregating results from all interfaces
	 *
	 * @param[in] discovery_time_ms Maximum time in milliseconds to wait for
	 *                              device responses. Default is 500ms.
	 * @return Map of discovered devices keyed by MAC address.
	 *         Each entry contains the full DeviceInfo for that device.
	 *
	 * @note This method blocks for approximately @p discovery_time_ms.
	 * @note Duplicate devices (same MAC) discovered on different interfaces
	 *       will only appear once in the result.
	 */
	std::map<std::string, DeviceInfo> DiscoverDevices(int discovery_time_ms = 500);

private:
	/// @brief Collection of Ethernet sockets, one per network interface
	std::vector<std::shared_ptr<EthernetSocket>> sockets_;

	/// @brief Map of discovered devices (MAC address -> DeviceInfo)
	std::map<std::string, DeviceInfo> discovered_devices_;

	/// @brief Mutex protecting access to discovered_devices_
	std::mutex devices_mutex_;

	/// @brief Flag indicating whether discovery is in progress
	std::atomic<bool> running_{ false };

	/// @brief Counter of interfaces currently performing discovery
	std::atomic<int> active_interfaces_{ 0 };

	/// @brief Pre-parsed broadcast MAC address (FF:FF:FF:FF:FF:FF)
	std::array<uint8_t, 6> broadcast_mac_{};

	/**
	 * @brief Performs device discovery on a single network interface.
	 *
	 * Creates a socket, sends discovery packets, and processes responses
	 * for the specified interface.
	 *
	 * @param[in] interface     Name of the network interface (e.g., "eth0")
	 * @param[in] local_mac     MAC address of the local interface
	 * @param[in] discovery_time_ms Time to wait for responses in milliseconds
	 */
	void DiscoverOnInterface(const std::string &interface, const std::string &local_mac, int discovery_time_ms);

	/**
	 * @brief Handles received packets from the Ethernet socket.
	 *
	 * This callback is invoked when data is received on a socket.
	 * It validates the packet and delegates to ParseResponse() for
	 * device information extraction.
	 *
	 * @param[in] socket_ptr Shared pointer to the socket that received data
	 * @param[in] data       Pointer to the received packet data
	 * @param[in] length     Length of the received data in bytes
	 * @param[in] ec         Boost error code indicating receive status
	 */
	void HandleReceive(const std::shared_ptr<EthernetSocket> &socket_ptr, const uint8_t *data, size_t length,
					   const boost::system::error_code &ec);

	/**
	 * @brief Parses a device discovery response packet.
	 *
	 * Extracts device information from a valid response packet and
	 * adds it to the discovered_devices_ map.
	 *
	 * @param[in] interface Name of the interface where packet was received
	 * @param[in] local_mac Local MAC address of the receiving interface
	 * @param[in] buffer    Pointer to the packet buffer
	 * @param[in] len       Length of the packet in bytes
	 * @return true if the packet was a valid discovery response and was parsed
	 *         successfully, false otherwise.
	 */
	bool ParseResponse(const std::string &interface, const MacAddress &local_mac, const uint8_t *buffer, size_t len);

	/**
	 * @brief Sends a discovery ping packet on the specified socket.
	 *
	 * Constructs and transmits a broadcast discovery packet to elicit
	 * responses from INS401 devices on the network.
	 *
	 * @param[in] socket_ptr Shared pointer to the socket for transmission
	 */
	void SendDiscoveryPing(const std::shared_ptr<EthernetSocket> &socket_ptr) const;
};
