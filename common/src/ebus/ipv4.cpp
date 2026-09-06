#include "ebus/ipv4.h"

#include <cstdio>

#include "string_util.h"

namespace common::Ebus {
    bool ParseIpv4(const std::string &text, uint32_t &out) {
        const std::vector<std::string> parts = common::StringUtil::Split(text, '.');
        if (parts.size() != 4) {
            return false;
        }
        uint32_t value = 0;
        for (const std::string &part: parts) {
            if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos ||
                (part.size() > 1 && part[0] == '0')) {
                return false; // no leading zeros: "010" is not an octet
            }
            const int octet = std::stoi(part);
            if (octet > 255) {
                return false;
            }
            value = (value << 8) | static_cast<uint32_t>(octet);
        }
        out = value;
        return true;
    }


    std::string FormatIpv4(uint32_t value) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (value >> 24) & 0xFFu, (value >> 16) & 0xFFu,
                      (value >> 8) & 0xFFu, value & 0xFFu);
        return buf;
    }


    bool IsContiguousMask(uint32_t mask) {
        // Inverting turns "ones then zeros" into "zeros then ones", which is
        // one less than a power of two.
        const uint32_t inv = ~mask;
        return (inv & (inv + 1u)) == 0u;
    }


    int PrefixLength(uint32_t mask) {
        if (!IsContiguousMask(mask)) {
            return -1;
        }
        int n = 0;
        for (uint32_t bit = 0x80000000u; bit != 0 && (mask & bit) != 0; bit >>= 1) {
            ++n;
        }
        return n;
    }


    bool SameSubnet(uint32_t a, uint32_t b, uint32_t mask) {
        return (a & mask) == (b & mask);
    }


    bool IsUsableHost(uint32_t ip, uint32_t mask) {
        if (ip == 0u) {
            return false;
        }
        if ((ip >> 24) == 127u || (ip >> 24) >= 224u) {
            return false; // loopback, multicast, reserved
        }
        if (mask != 0xFFFFFFFFu && mask != 0u) {
            if ((ip & mask) == ip) {
                return false; // network address
            }
            if ((ip | ~mask) == ip) {
                return false; // directed broadcast
            }
        }
        return true;
    }


    std::string ValidateSetIp(const std::string &ip, const std::string &subnet_mask, const std::string &gateway,
                                const std::vector<HostAddress> &host_addresses, bool allow_foreign_subnet) {
        uint32_t ip_v = 0, mask_v = 0, gw_v = 0;
        if (!ParseIpv4(ip, ip_v)) {
            return "\"" + ip + "\" is not an IPv4 address";
        }
        if (!ParseIpv4(subnet_mask, mask_v)) {
            return "\"" + subnet_mask + "\" is not an IPv4 subnet mask";
        }
        if (!ParseIpv4(gateway, gw_v)) {
            return "\"" + gateway + "\" is not an IPv4 gateway address";
        }
        const int prefix = PrefixLength(mask_v);
        if (prefix < 8 || prefix > 30) {
            return "subnet mask " + subnet_mask + " must be contiguous and between /8 and /30";
        }
        if (!IsUsableHost(ip_v, mask_v)) {
            return ip + "/" + std::to_string(prefix) +
                   " is not a usable host address (network, broadcast, loopback, multicast or 0.0.0.0)";
        }
        if (gw_v != 0u) {
            if (!SameSubnet(gw_v, ip_v, mask_v) || !IsUsableHost(gw_v, mask_v)) {
                return "gateway " + gateway + " is not a host on " + FormatIpv4(ip_v & mask_v) + "/" +
                       std::to_string(prefix);
            }
            if (gw_v == ip_v) {
                return "gateway " + gateway + " is the camera address itself";
            }
        }

        bool reachable = false;
        std::string host_list;
        for (const HostAddress &h: host_addresses) {
            uint32_t h_ip = 0, h_mask = 0;
            if (!ParseIpv4(h.ip, h_ip) || !ParseIpv4(h.subnet_mask, h_mask)) {
                continue;
            }
            if (!host_list.empty()) {
                host_list += ", ";
            }
            host_list += h.ip + "/" + h.subnet_mask;
            if (h_ip == ip_v) {
                return ip + " is the address of the host adapter itself";
            }
            // Both sides must agree that they share a subnet, or one of them
            // will route the other's packets through a gateway that is not there.
            if (SameSubnet(h_ip, ip_v, h_mask) && SameSubnet(h_ip, ip_v, mask_v)) {
                reachable = true;
            }
        }
        if (!reachable && !allow_foreign_subnet) {
            return ip + "/" + std::to_string(prefix) +
                   " is not on a subnet of the host adapter that sees this camera (host: " +
                   (host_list.empty() ? std::string("no IPv4 address") : host_list) +
                   "); the camera would become unreachable from this machine. Pick an address in one of those "
                   "subnets, or pass --allow-foreign-subnet if the host will be re-addressed afterwards";
        }
        return "";
    }
} // namespace common::Ebus
