// Pixel-format table + GVSP legacy unpack (2 pixels in 3 bytes). Bit layouts
// per the GigE Vision spec:
//   Mono12Packed: b0 = P0[11:4], b1 = (P1[3:0] << 4) | P0[3:0], b2 = P1[11:4]
//   Mono10Packed: b0 = P0[9:2],  b1 bits[1:0] = P0[1:0], bits[5:4] = P1[1:0], b2 = P1[9:2]

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../include/pixel_format.hpp"

using namespace fx10;

TEST(PixelFormat, Table) {
	ASSERT_NE(pixelFormatInfo("Mono8"), nullptr);
	EXPECT_EQ(pixelFormatInfo("Mono8")->storage_bpp, 1u);
	EXPECT_FALSE(pixelFormatInfo("Mono8")->packed);

	ASSERT_NE(pixelFormatInfo("Mono12"), nullptr);
	EXPECT_EQ(pixelFormatInfo("Mono12")->storage_bpp, 2u);
	EXPECT_FALSE(pixelFormatInfo("Mono12")->packed);

	ASSERT_NE(pixelFormatInfo("Mono12Packed"), nullptr);
	EXPECT_TRUE(pixelFormatInfo("Mono12Packed")->packed);
	EXPECT_EQ(pixelFormatInfo("Mono12Packed")->storage_bpp, 2u);

	ASSERT_NE(pixelFormatInfo("Mono10Packed"), nullptr);
	EXPECT_TRUE(pixelFormatInfo("Mono10Packed")->packed);

	EXPECT_EQ(pixelFormatInfo("Mono16"), nullptr);   // deliberately unsupported
	EXPECT_EQ(pixelFormatInfo("Mono12p"), nullptr);  // PFNC flavor, not the GVSP one
}

TEST(PixelFormat, WireBytes) {
	const auto &mono12 = *pixelFormatInfo("Mono12");
	const auto &packed12 = *pixelFormatInfo("Mono12Packed");
	const auto &mono8 = *pixelFormatInfo("Mono8");
	EXPECT_EQ(wireBytes(mono12, 1024u * 448u), 1024u * 448u * 2u);
	EXPECT_EQ(wireBytes(mono8, 1024u * 448u), 1024u * 448u);
	EXPECT_EQ(wireBytes(packed12, 1024u * 448u), 1024u * 448u * 3u / 2u);  // 1.5 B/px
	EXPECT_EQ(wireBytes(packed12, 2), 3u);
	EXPECT_EQ(wireBytes(packed12, 3), 6u);  // defensive odd tail: full 3-byte group
}

TEST(PixelFormat, UnpackMono12Packed) {
	// P0 = 0xABC, P1 = 0x123 -> b0 = 0xAB, b1 = (0x3 << 4) | 0xC = 0x3C, b2 = 0x12
	const std::uint8_t src[] = { 0xAB, 0x3C, 0x12 };
	std::uint16_t dst[2] = {};
	unpackMono12Packed(src, 2, dst);
	EXPECT_EQ(dst[0], 0x0ABC);
	EXPECT_EQ(dst[1], 0x0123);
}

TEST(PixelFormat, UnpackMono12PackedExtremes) {
	// P0 = 0xFFF, P1 = 0x000 and the reverse
	const std::uint8_t max_min[] = { 0xFF, 0x0F, 0x00 };
	std::uint16_t dst[2] = {};
	unpackMono12Packed(max_min, 2, dst);
	EXPECT_EQ(dst[0], 0x0FFF);
	EXPECT_EQ(dst[1], 0x0000);

	const std::uint8_t min_max[] = { 0x00, 0xF0, 0xFF };
	unpackMono12Packed(min_max, 2, dst);
	EXPECT_EQ(dst[0], 0x0000);
	EXPECT_EQ(dst[1], 0x0FFF);
}

TEST(PixelFormat, UnpackMono12PackedRun) {
	// 6 pixels = 3 groups; values chosen to catch group-offset mistakes
	const std::uint16_t expect[] = { 0x111, 0x222, 0x333, 0x444, 0x555, 0x666 };
	std::vector<std::uint8_t> src;
	for (int g = 0; g < 3; ++g) {
		const std::uint16_t p0 = expect[g * 2];
		const std::uint16_t p1 = expect[g * 2 + 1];
		src.push_back(static_cast<std::uint8_t>(p0 >> 4));
		src.push_back(static_cast<std::uint8_t>(((p1 & 0x0F) << 4) | (p0 & 0x0F)));
		src.push_back(static_cast<std::uint8_t>(p1 >> 4));
	}
	std::uint16_t dst[6] = {};
	unpackMono12Packed(src.data(), 6, dst);
	for (int i = 0; i < 6; ++i) {
		EXPECT_EQ(dst[i], expect[i]) << "pixel " << i;
	}
}

TEST(PixelFormat, UnpackMono10Packed) {
	// P0 = 0x2AB (0b10'1010'1011), P1 = 0x155 (0b01'0101'0101)
	// b0 = P0[9:2] = 0xAA, b1 = (P1[1:0] << 4) | P0[1:0] = 0x13, b2 = P1[9:2] = 0x55
	const std::uint8_t src[] = { 0xAA, 0x13, 0x55 };
	std::uint16_t dst[2] = {};
	unpackMono10Packed(src, 2, dst);
	EXPECT_EQ(dst[0], 0x2AB);
	EXPECT_EQ(dst[1], 0x155);
}

TEST(PixelFormat, UnpackMono10PackedExtremes) {
	// P0 = 0x3FF, P1 = 0x000
	const std::uint8_t max_min[] = { 0xFF, 0x03, 0x00 };
	std::uint16_t dst[2] = {};
	unpackMono10Packed(max_min, 2, dst);
	EXPECT_EQ(dst[0], 0x3FF);
	EXPECT_EQ(dst[1], 0x000);
}
