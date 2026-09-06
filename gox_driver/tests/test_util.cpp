// UUID and hex helpers (src/util.cpp). The session UUID lands verbatim in
// every segment file header, so its RFC 4122 shape is a format concern.

#include "util.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <string>

TEST_CASE("util: gen_uuid_v4 produces a well-formed version-4 UUID") {
    uint8_t uuid[16];
    for (int round = 0; round < 32; ++round) {
        CAPTURE(round);
        gox::GenUuidV4(uuid);
        CHECK((uuid[6] & 0xF0) == 0x40); // version 4
        CHECK((uuid[8] & 0xC0) == 0x80); // variant 10xx
    }
}

TEST_CASE("util: successive UUIDs differ") {
    std::set<std::string> seen;
    for (int i = 0; i < 16; ++i) {
        uint8_t uuid[16];
        gox::GenUuidV4(uuid);
        seen.insert(gox::HexPrefix(uuid, 16));
    }
    CHECK(seen.size() == 16u);
}

TEST_CASE("util: hex_prefix is lowercase, zero padded and length exact") {
    const uint8_t bytes[] = {0x00, 0x0f, 0xa0, 0xff, 0x5a};
    CHECK(gox::HexPrefix(bytes, 5) == "000fa0ff5a");
    CHECK(gox::HexPrefix(bytes, 3) == "000fa0"); // the session-name prefix length
    CHECK(gox::HexPrefix(bytes, 0).empty());
}
