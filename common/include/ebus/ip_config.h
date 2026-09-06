#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ebus/discovery.h"

namespace common::Ebus {
    struct SetIpRequest {
        std::string mac; // any MAC spelling; matched after NormalizeMac
        std::string ip;
        std::string subnet_mask;
        std::string gateway = "0.0.0.0";
        uint32_t discovery_timeout_ms = kDiscoveryTimeoutMs;
        uint32_t reachable_timeout_ms = 10000; // how long to wait for the camera to reappear after FORCEIP
        bool allow_foreign_subnet = false; // skip the "reachable from this host" check
    };

    // Exit codes of ebus_set_ip (also SetIpReport::exit_code).
    enum SetIpExit : int {
        kSetIpOk = 0,
        kSetIpBadRequest = 2, // arguments / validation
        kSetIpNotFound = 3, // no device with that MAC answered discovery
        kSetIpForceIpRejected = 4,
        kSetIpUnreachable = 5, // FORCEIP sent, camera never reappeared at the new address
        kSetIpPersistFailed = 6, // new address active but NOT persisted (unsupported or write failed)
        kSetIpSdkError = 7, // discovery / connect failure
    };

    struct IpState {
        std::string ip;
        std::string subnet_mask;
        std::string gateway;
        bool configuration_valid = false;
        bool persistent_available = false;
        bool persistent_enabled = false;
        bool dhcp_enabled = false;
    };

    struct PersistentWrite {
        bool attempted = false;
        bool supported = false; // camera advertises persistent IP and the nodes exist
        bool written = false; // all four read-backs match
        IpState readback; // GevPersistent* + PersistentIP flag as read back
        std::string error;
    };

    struct SetIpReport {
        int exit_code = kSetIpOk;
        std::string error; // "" on success
        SetIpRequest request;
        DiscoveredDevice device; // as found before the change
        std::vector<HostAddress> host_subnets; // addresses of the adapter that saw it
        bool forceip_sent = false;
        uint32_t reappeared_after_ms = 0;
        bool reappeared = false;
        IpState after_forceip; // the camera's state once it reappeared
        PersistentWrite persistent;
    };

    SetIpReport SetDeviceIp(const SetIpRequest &request);

    nlohmann::ordered_json SetIpReportToJson(const SetIpReport &report);
} // namespace common::Ebus
