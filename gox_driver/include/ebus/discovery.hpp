#pragma once

// GigE Vision device discovery and selector matching on top of PvSystem.
// Discovery is needed only for selectors that cannot be dialled directly
// (mac / serial / user_defined_name): a PvSystem::Find() pass blocks for its
// FULL detection timeout, so by=ip skips it and connects straight away
// (fx10-aligned fast path). The matched device is returned as a plain-data
// snapshot; connect through its connection_id.

#include <string>

#include "app_config.hpp"


namespace jai::ebus {
    // Plain-data snapshot of one discovered GEV device.
    struct DiscoveredDevice {
        std::string connection_id; // PvDeviceInfo::GetConnectionID(), used for Connect-by-string
        std::string ip;
        std::string mac;
        std::string serial;
        std::string model;
        std::string vendor;
        std::string user_name;
        std::string firmware; // PvDeviceInfo::GetVersion()
        std::string interface_name; // NIC the device was seen on
        bool configuration_valid = true; // PvDeviceInfoGEV::IsConfigurationValid()
    };

    // Full PvSystem::Find() with the configured detection timeout and retry
    // schedule, then selector matching (by mac / serial / user_defined_name /
    // ip; MAC compared after Common::StringUtil::NormalizeMac on both sides).
    // Sends a FORCEIP rescue and re-runs the pass when the match sits on a
    // wrong subnet and force_ip is enabled. Throws std::runtime_error on: no
    // match after all retries (after logging every discovered device), more
    // than one match, or an invalid subnet configuration that force_ip could
    // not (or was not allowed to) repair.
    DiscoveredDevice find_camera(const SelectorConfig &selector, const DiscoveryConfig &discovery);
} // namespace jai::ebus
