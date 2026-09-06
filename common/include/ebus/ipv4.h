#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace common::Ebus {
    // Dotted quad <-> host-order value (a<<24 | b<<16 | c<<8 | d).
    bool ParseIpv4(const std::string &text, uint32_t &out);

    std::string FormatIpv4(uint32_t value);

    // Ones followed by zeros (0.0.0.0 and 255.255.255.255 count as contiguous).
    bool IsContiguousMask(uint32_t mask);

    // Number of leading ones, or -1 when the mask is not contiguous.
    int PrefixLength(uint32_t mask);

    bool SameSubnet(uint32_t a, uint32_t b, uint32_t mask);

    bool IsUsableHost(uint32_t ip, uint32_t mask);

    // One address of a host network adapter (what PvNetworkAdapter reports).
    struct HostAddress {
        std::string ip;
        std::string subnet_mask;
    };

    std::string ValidateSetIp(const std::string &ip, const std::string &subnet_mask, const std::string &gateway,
                                const std::vector<HostAddress> &host_addresses, bool allow_foreign_subnet);
} // namespace common::Ebus
