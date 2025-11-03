#pragma once

#include <iostream>
#include <iomanip>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <map>
#include <sstream>
#include <algorithm>
#include <cerrno>

#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "data_type.h"
#include "tool.h"



const std::string BROADCAST_MAC = "FF:FF:FF:FF:FF:FF";


class INSDeviceDiscovery {
public:
    INSDeviceDiscovery();
    ~INSDeviceDiscovery();

    std::map<std::string, DeviceInfo> GetDiscoveredDevices();
    void ClearDiscoveredDevices();


private:
    int raw_socket_;
    uint8_t *BROADCAST_MAC_;
    uint8_t *COMMAND_START_;
    uint8_t *REQUEST_INFO_COMMAND_;
    std::map<std::string, DeviceInfo> discovered_devices;
    bool discovery_running;

    bool CreateRawSocket(const std::string &interface);
    std::vector<uint8_t> BuildPingPacket(const uint8_t *src_mac);
    bool ParseResponse(const uint8_t *buffer, size_t len);
    void ListenforResponses(int timeout_ms=500);
    bool SendBroadcastPing(const std::string &interface, const std::string &mac_str);
    void DiscoverDevices(int discovery_time_ms=1500);
};