#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ebus/ipv4.h"

namespace common::Ebus {
    // Plain-data snapshot of one discovered GEV device.
    struct DiscoveredDevice {
        std::string connection_id; // PvDeviceInfo::GetConnectionID(), used for Connect-by-string
        std::string display_id; // PvDeviceInfo::GetDisplayID()
        std::string ip;
        std::string subnet_mask;
        std::string gateway;
        std::string mac; // as the SDK prints it ("00:0C:DF:12:34:56"); SetIPConfiguration wants this form
        std::string serial;
        std::string model;
        std::string vendor;
        std::string user_name;
        std::string firmware; // PvDeviceInfo::GetVersion()
        std::string interface_name; // NIC the device was seen on
        bool configuration_valid = true; // PvDeviceInfo::IsConfigurationValid(): reachable from its NIC
        // IP configuration capabilities/state (PvDeviceInfoGEV::Is*)
        bool persistent_available = false;
        bool persistent_enabled = false;
        bool dhcp_enabled = false;
        bool lla_enabled = false;
    };

    // One host network adapter and everything found on it.
    struct HostAdapter {
        std::string name;
        std::string mac;
        std::vector<HostAddress> addresses;
        std::vector<DiscoveredDevice> devices;
        std::vector<std::string> non_gev_display_ids; // seen, but not GigE Vision
    };

    struct DiscoveryResult {
        std::vector<HostAdapter> interfaces;

        [[nodiscard]] size_t GetDeviceCount() const;
    };

    // Detection window both drivers have always used for their MAC resolve.
    constexpr uint32_t kDiscoveryTimeoutMs = 4000;

    // One PvSystem::Find() pass over every interface.
    // Throws SdkError when the SDK refuses to search at all. Does not log.
    DiscoveryResult DiscoverDevices(uint32_t timeout_ms);

    DiscoveredDevice FindCamera(const std::string &mac, uint32_t timeout_ms = kDiscoveryTimeoutMs);

    // One-line renderings for logs and error messages.
    std::string DeviceLine(const DiscoveredDevice &d);

    std::string AdapterLine(const HostAdapter &a); // "eth0 10.95.1.1/255.255.255.0 ..."
} // namespace common::Ebus
