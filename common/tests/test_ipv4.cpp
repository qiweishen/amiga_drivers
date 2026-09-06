/// @file test_ipv4.cpp
/// @brief Tests for the SDK-free IPv4 helpers behind ebus_set_ip
/// (common/include/ebus/ipv4.hpp): parsing, subnet arithmetic and the full
/// set-IP request validation.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ebus/ipv4.h"

using common::Ebus::HostAddress;

namespace {
    uint32_t ip(const std::string &text) {
        uint32_t v = 0;
        REQUIRE(common::Ebus::ParseIpv4(text, v));
        return v;
    }
} // namespace


TEST_CASE("ipv4: parse and format round-trip") {
    CHECK(ip("10.95.1.101") == 0x0A5F0165u);
    CHECK(ip("0.0.0.0") == 0u);
    CHECK(ip("255.255.255.255") == 0xFFFFFFFFu);
    CHECK(common::Ebus::FormatIpv4(0x0A5F0165u) == "10.95.1.101");
    CHECK(common::Ebus::FormatIpv4(0u) == "0.0.0.0");

    uint32_t v = 0;
    for (const char *bad: {"", "10.95.1", "10.95.1.101.1", "10.95.1.256", "10.95.1.-1", "10.95.1.a", "10..1.1",
                           " 10.95.1.101", "10.95.1.101 ", "1e3.0.0.1",
                           "010.095.001.101", "10.95.1.01"}) {
        CAPTURE(bad);
        CHECK_FALSE(common::Ebus::ParseIpv4(bad, v));
    }
}


TEST_CASE("ipv4: subnet masks") {
    CHECK(common::Ebus::IsContiguousMask(ip("255.255.255.0")));
    CHECK(common::Ebus::IsContiguousMask(ip("255.255.254.0")));
    CHECK(common::Ebus::IsContiguousMask(ip("255.0.0.0")));
    CHECK(common::Ebus::IsContiguousMask(ip("255.255.255.255")));
    CHECK(common::Ebus::IsContiguousMask(ip("0.0.0.0")));
    CHECK_FALSE(common::Ebus::IsContiguousMask(ip("255.0.255.0")));
    CHECK_FALSE(common::Ebus::IsContiguousMask(ip("255.255.255.1")));

    CHECK(common::Ebus::PrefixLength(ip("255.255.255.0")) == 24);
    CHECK(common::Ebus::PrefixLength(ip("255.255.254.0")) == 23);
    CHECK(common::Ebus::PrefixLength(ip("255.255.255.252")) == 30);
    CHECK(common::Ebus::PrefixLength(ip("255.255.255.255")) == 32);
    CHECK(common::Ebus::PrefixLength(ip("0.0.0.0")) == 0);
    CHECK(common::Ebus::PrefixLength(ip("255.0.255.0")) == -1);

    CHECK(common::Ebus::SameSubnet(ip("10.95.1.1"), ip("10.95.1.200"), ip("255.255.255.0")));
    CHECK_FALSE(common::Ebus::SameSubnet(ip("10.95.1.1"), ip("10.95.2.1"), ip("255.255.255.0")));
    CHECK(common::Ebus::SameSubnet(ip("10.95.1.1"), ip("10.95.2.1"), ip("255.255.0.0")));
}


TEST_CASE("ipv4: usable host addresses") {
    const uint32_t mask24 = ip("255.255.255.0");
    CHECK(common::Ebus::IsUsableHost(ip("10.95.1.101"), mask24));
    CHECK(common::Ebus::IsUsableHost(ip("10.95.1.1"), mask24));
    CHECK(common::Ebus::IsUsableHost(ip("10.95.1.254"), mask24));
    CHECK_FALSE(common::Ebus::IsUsableHost(ip("10.95.1.0"), mask24)); // network
    CHECK_FALSE(common::Ebus::IsUsableHost(ip("10.95.1.255"), mask24)); // broadcast
    CHECK_FALSE(common::Ebus::IsUsableHost(ip("0.0.0.0"), mask24));
    CHECK_FALSE(common::Ebus::IsUsableHost(ip("127.0.0.1"), mask24)); // loopback
    CHECK_FALSE(common::Ebus::IsUsableHost(ip("224.0.0.1"), mask24)); // multicast
    CHECK_FALSE(common::Ebus::IsUsableHost(ip("240.0.0.1"), mask24)); // reserved
    // /23 (10.95.0.0-10.95.1.255): 10.95.0.255 and 10.95.1.0 are ordinary
    // hosts; the edges move to 10.95.0.0 / 10.95.1.255.
    const uint32_t mask23 = ip("255.255.254.0");
    CHECK(common::Ebus::IsUsableHost(ip("10.95.0.255"), mask23));
    CHECK(common::Ebus::IsUsableHost(ip("10.95.1.0"), mask23));
    CHECK_FALSE(common::Ebus::IsUsableHost(ip("10.95.0.0"), mask23)); // network
    CHECK_FALSE(common::Ebus::IsUsableHost(ip("10.95.1.255"), mask23)); // broadcast
}


TEST_CASE("ipv4: validate_set_ip accepts a reachable, well-formed request") {
    const std::vector<HostAddress> host = {{"10.95.1.1", "255.255.255.0"}};
    CHECK(common::Ebus::ValidateSetIp("10.95.1.101", "255.255.255.0", "0.0.0.0", host, false).empty());
    // A gateway on the subnet is fine
    CHECK(common::Ebus::ValidateSetIp("10.95.1.101", "255.255.255.0", "10.95.1.254", host, false).empty());
    // Two host addresses: only one has to match
    const std::vector<HostAddress> two = {{"192.168.0.10", "255.255.255.0"}, {"10.95.1.1", "255.255.255.0"}};
    CHECK(common::Ebus::ValidateSetIp("10.95.1.101", "255.255.255.0", "0.0.0.0", two, false).empty());
}


TEST_CASE("ipv4: validate_set_ip rejects what would brick the link") {
    const std::vector<HostAddress> host = {{"10.95.1.1", "255.255.255.0"}};
    using common::Ebus::ValidateSetIp;

    SUBCASE("malformed inputs name the offending field") {
        CHECK(ValidateSetIp("10.95.1", "255.255.255.0", "0.0.0.0", host, false).find("IPv4 address") !=
              std::string::npos);
        CHECK(ValidateSetIp("10.95.1.101", "255.255.255", "0.0.0.0", host, false).find("subnet mask") !=
              std::string::npos);
        CHECK(ValidateSetIp("10.95.1.101", "255.255.255.0", "gw", host, false).find("gateway") !=
              std::string::npos);
    }
    SUBCASE("mask must be contiguous and /8../30") {
        CHECK_FALSE(ValidateSetIp("10.95.1.101", "255.0.255.0", "0.0.0.0", host, false).empty());
        CHECK_FALSE(ValidateSetIp("10.95.1.101", "255.255.255.254", "0.0.0.0", host, false).empty()); // /31
        CHECK_FALSE(ValidateSetIp("10.95.1.101", "255.255.255.255", "0.0.0.0", host, false).empty()); // /32
        CHECK_FALSE(ValidateSetIp("10.95.1.101", "254.0.0.0", "0.0.0.0", host, false).empty()); // /7
    }
    SUBCASE("network, broadcast, loopback and multicast are not hosts") {
        CHECK_FALSE(ValidateSetIp("10.95.1.0", "255.255.255.0", "0.0.0.0", host, false).empty());
        CHECK_FALSE(ValidateSetIp("10.95.1.255", "255.255.255.0", "0.0.0.0", host, false).empty());
        CHECK_FALSE(ValidateSetIp("127.0.0.5", "255.0.0.0", "0.0.0.0", {{"127.0.0.1", "255.0.0.0"}}, true).empty());
        CHECK_FALSE(ValidateSetIp("224.0.0.5", "255.255.255.0", "0.0.0.0", host, true).empty());
    }
    SUBCASE("gateway must be another host on the same subnet") {
        CHECK_FALSE(ValidateSetIp("10.95.1.101", "255.255.255.0", "10.95.2.1", host, false).empty());
        CHECK_FALSE(ValidateSetIp("10.95.1.101", "255.255.255.0", "10.95.1.101", host, false).empty());
        CHECK_FALSE(ValidateSetIp("10.95.1.101", "255.255.255.0", "10.95.1.255", host, false).empty());
    }
    SUBCASE("foreign subnet is refused unless explicitly allowed, and the host subnets are listed") {
        const std::string why = ValidateSetIp("192.168.0.100", "255.255.255.0", "0.0.0.0", host, false);
        CHECK_FALSE(why.empty());
        CHECK(why.find("10.95.1.1/255.255.255.0") != std::string::npos);
        CHECK(why.find("--allow-foreign-subnet") != std::string::npos);
        CHECK(ValidateSetIp("192.168.0.100", "255.255.255.0", "0.0.0.0", host, true).empty());
        // Same network address but a mask the host does not share is foreign too
        CHECK_FALSE(ValidateSetIp("10.95.3.101", "255.255.0.0", "0.0.0.0", host, false).empty());
    }
    SUBCASE("colliding with the host adapter is refused even with the override") {
        CHECK_FALSE(ValidateSetIp("10.95.1.1", "255.255.255.0", "0.0.0.0", host, true).empty());
    }
    SUBCASE("a host without an IPv4 address cannot reach anything") {
        const std::string why = ValidateSetIp("10.95.1.101", "255.255.255.0", "0.0.0.0", {}, false);
        CHECK(why.find("no IPv4 address") != std::string::npos);
        CHECK(ValidateSetIp("10.95.1.101", "255.255.255.0", "0.0.0.0", {}, true).empty());
    }
}
