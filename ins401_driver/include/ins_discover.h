#ifndef INS_DISCOVER_H
#define INS_DISCOVER_H

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ethernet_socket.h"


// Information reported by a discovered device.
struct DeviceInfo {
    std::string interface_name;
    std::string mac_address;
    std::string localhost_mac_address;
    std::string product = "INS401";
    std::string part_number;
    std::string serial_number;
    std::string hardware_version;
    std::string imu_serial_number;
    std::string firmware_version;
    std::string bootloader_version;
    std::string imu_firmware_version;
    std::string gnss_chip_firmware_version;
};

// Discovers INS401 devices on available network interfaces.
class INSDeviceDiscover {
public:
    explicit INSDeviceDiscover();

    ~INSDeviceDiscover();

    std::map<std::string, DeviceInfo> DiscoverDevices(int discovery_time_ms = 500);

private:
    std::vector<std::shared_ptr<EthernetSocket> > sockets_;

    std::map<std::string, DeviceInfo> discovered_devices_;

    std::mutex devices_mutex_;

    std::atomic<bool> running_{false};

    std::atomic<int> active_interfaces_{0};

    std::array<uint8_t, 6> broadcast_mac_{};

    void DiscoverOnInterface(const std::string &interface, const std::string &local_mac, int discovery_time_ms);

    void HandleReceive(const std::shared_ptr<EthernetSocket> &socket_ptr, const uint8_t *data, size_t length,
                       const boost::system::error_code &ec);

    bool ParseResponse(const std::string &interface, const MacAddress &local_mac, const uint8_t *buffer, size_t len);

    void SendDiscoveryPing(const std::shared_ptr<EthernetSocket> &socket_ptr) const;
};


#endif