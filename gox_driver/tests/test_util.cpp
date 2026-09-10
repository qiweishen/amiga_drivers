// UUID and hex helpers (src/util.cpp). The session UUID lands verbatim in
// every segment file header, so its RFC 4122 shape is a format concern.

#include "util.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

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

TEST_CASE("util: metadata publication preserves existing snapshots and temporary files") {
    namespace fs = std::filesystem;
    uint8_t uuid[16];
    gox::GenUuidV4(uuid);
    const auto dir = fs::temp_directory_path() / ("gox_metadata_" + gox::HexPrefix(uuid, 16));
    REQUIRE(fs::create_directory(dir));
    const auto path = dir / "device.json";
    const std::string original = "{\"serial\":\"unit\"}\n";
    gox::PublishMetadata(path.string(), original);
    CHECK_FALSE(fs::exists(path.string() + ".part"));
    CHECK_THROWS_AS(gox::PublishMetadata(path.string(), "replacement"), std::runtime_error);
    std::ifstream input(path);
    CHECK(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) == original);
    input.close();
    const auto other = dir / "other.json";
    { std::ofstream(other.string() + ".part") << "existing temporary data"; }
    CHECK_THROWS_AS(gox::PublishMetadata(other.string(), "new"), std::runtime_error);
    CHECK(fs::file_size(other.string() + ".part") == 23);
    CHECK_FALSE(fs::exists(other));
    fs::remove_all(dir);
}
