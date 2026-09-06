// Pixel-format table + GVSP legacy unpack (2 pixels in 3 bytes). Bit layouts
// per the GigE Vision spec:
//   Mono12Packed: b0 = P0[11:4], b1 = (P1[3:0] << 4) | P0[3:0], b2 = P1[11:4]
//   Mono10Packed: b0 = P0[9:2],  b1 bits[1:0] = P0[1:0], bits[5:4] = P1[1:0], b2 = P1[9:2]

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "../include/pixel_format.h"

using namespace fx10;

TEST_CASE("PixelFormat: Table") {
    REQUIRE(GetPixelFormatInfo("Mono8") != nullptr);
    CHECK(GetPixelFormatInfo("Mono8")->storage_bpp == 1u);
    CHECK_FALSE(GetPixelFormatInfo("Mono8")->packed);

    REQUIRE(GetPixelFormatInfo("Mono12") != nullptr);
    CHECK(GetPixelFormatInfo("Mono12")->storage_bpp == 2u);
    CHECK_FALSE(GetPixelFormatInfo("Mono12")->packed);

    REQUIRE(GetPixelFormatInfo("Mono12Packed") != nullptr);
    CHECK(GetPixelFormatInfo("Mono12Packed")->packed);
    CHECK(GetPixelFormatInfo("Mono12Packed")->storage_bpp == 2u);

    REQUIRE(GetPixelFormatInfo("Mono10Packed") != nullptr);
    CHECK(GetPixelFormatInfo("Mono10Packed")->packed);

    CHECK(GetPixelFormatInfo("Mono16") == nullptr); // deliberately unsupported
    CHECK(GetPixelFormatInfo("Mono12p") == nullptr); // PFNC flavor, not the GVSP one
}

TEST_CASE("PixelFormat: WireBytes") {
    const auto &mono12 = *GetPixelFormatInfo("Mono12");
    const auto &packed12 = *GetPixelFormatInfo("Mono12Packed");
    const auto &mono8 = *GetPixelFormatInfo("Mono8");
    CHECK(WireBytes(mono12, 1024u * 448u) == 1024u * 448u * 2u);
    CHECK(WireBytes(mono8, 1024u * 448u) == 1024u * 448u);
    CHECK(WireBytes(packed12, 1024u * 448u) == 1024u * 448u * 3u / 2u); // 1.5 B/px
    CHECK(WireBytes(packed12, 2) == 3u);
    CHECK(WireBytes(packed12, 3) == 6u); // defensive odd tail: full 3-byte group
}

TEST_CASE("PixelFormat: UnpackMono12Packed") {
    // P0 = 0xABC, P1 = 0x123 -> b0 = 0xAB, b1 = (0x3 << 4) | 0xC = 0x3C, b2 = 0x12
    const std::uint8_t src[] = {0xAB, 0x3C, 0x12};
    std::uint16_t dst[2] = {};
    UnpackMono12Packed(src, 2, dst);
    CHECK(dst[0] == 0x0ABC);
    CHECK(dst[1] == 0x0123);
}

TEST_CASE("PixelFormat: UnpackMono12PackedExtremes") {
    // P0 = 0xFFF, P1 = 0x000 and the reverse
    const std::uint8_t max_min[] = {0xFF, 0x0F, 0x00};
    std::uint16_t dst[2] = {};
    UnpackMono12Packed(max_min, 2, dst);
    CHECK(dst[0] == 0x0FFF);
    CHECK(dst[1] == 0x0000);

    const std::uint8_t min_max[] = {0x00, 0xF0, 0xFF};
    UnpackMono12Packed(min_max, 2, dst);
    CHECK(dst[0] == 0x0000);
    CHECK(dst[1] == 0x0FFF);
}

TEST_CASE("PixelFormat: UnpackMono12PackedRun") {
    // 6 pixels = 3 groups; values chosen to catch group-offset mistakes
    const std::uint16_t expect[] = {0x111, 0x222, 0x333, 0x444, 0x555, 0x666};
    std::vector<std::uint8_t> src;
    for (int g = 0; g < 3; ++g) {
        const std::uint16_t p0 = expect[g * 2];
        const std::uint16_t p1 = expect[g * 2 + 1];
        src.push_back(static_cast<std::uint8_t>(p0 >> 4));
        src.push_back(static_cast<std::uint8_t>(((p1 & 0x0F) << 4) | (p0 & 0x0F)));
        src.push_back(static_cast<std::uint8_t>(p1 >> 4));
    }
    std::uint16_t dst[6] = {};
    UnpackMono12Packed(src.data(), 6, dst);
    for (int i = 0; i < 6; ++i) {
        CHECK_MESSAGE((dst[i] == expect[i]), "pixel " << i);
    }
}

TEST_CASE("PixelFormat: UnpackMono10Packed") {
    // P0 = 0x2AB (0b10'1010'1011), P1 = 0x155 (0b01'0101'0101)
    // b0 = P0[9:2] = 0xAA, b1 = (P1[1:0] << 4) | P0[1:0] = 0x13, b2 = P1[9:2] = 0x55
    const std::uint8_t src[] = {0xAA, 0x13, 0x55};
    std::uint16_t dst[2] = {};
    UnpackMono10Packed(src, 2, dst);
    CHECK(dst[0] == 0x2AB);
    CHECK(dst[1] == 0x155);
}

TEST_CASE("PixelFormat: UnpackMono10PackedExtremes") {
    // P0 = 0x3FF, P1 = 0x000
    const std::uint8_t max_min[] = {0xFF, 0x03, 0x00};
    std::uint16_t dst[2] = {};
    UnpackMono10Packed(max_min, 2, dst);
    CHECK(dst[0] == 0x3FF);
    CHECK(dst[1] == 0x000);
}

TEST_CASE("PixelFormat: UnpackOddTail") {
    // 3 pixels in 2 groups; the second group carries only P2
    const std::uint8_t src12[] = {0xAB, 0x3C, 0x12, 0x45, 0x06, 0x00};
    std::uint16_t dst12[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    UnpackMono12Packed(src12, 3, dst12);
    CHECK(dst12[0] == 0xABC);
    CHECK(dst12[1] == 0x123);
    CHECK(dst12[2] == 0x456);

    const std::uint8_t src10[] = {0xAA, 0x13, 0x55, 0xFF, 0x03, 0x00};
    std::uint16_t dst10[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    UnpackMono10Packed(src10, 3, dst10);
    CHECK(dst10[0] == 0x2AB);
    CHECK(dst10[1] == 0x155);
    CHECK(dst10[2] == 0x3FF);
}

TEST_CASE("PixelFormat: UnpackIgnoresTheUnusedNibbleBits") {
    // Mono12Packed b1 carries both low nibbles; Mono10Packed b1 has unused bits 2-3 and 6-7
    const std::uint8_t src12[] = {0xAB, 0xDC, 0x12};
    std::uint16_t dst12[2] = {};
    UnpackMono12Packed(src12, 2, dst12);
    CHECK(dst12[0] == 0xABC);
    CHECK(dst12[1] == 0x12D);

    const std::uint8_t src10[] = {0xAA, 0xD3, 0x55}; // 0xD3 = 11'01'00'11: bits 2-3 and 6-7 must be masked
    std::uint16_t dst10[2] = {};
    UnpackMono10Packed(src10, 2, dst10);
    CHECK(dst10[0] == 0x2AB);
    CHECK(dst10[1] == 0x155);

    const std::uint8_t min_max[] = {0x00, 0x30, 0xFF}; // P0 = 0x000, P1 = 0x3FF
    UnpackMono10Packed(min_max, 2, dst10);
    CHECK(dst10[0] == 0x000);
    CHECK(dst10[1] == 0x3FF);
}
