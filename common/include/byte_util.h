/// @file byte_util.h
/// @brief Endian-explicit integer load/store helpers (header-only).

#ifndef COMMON_BYTE_UTIL_H
#define COMMON_BYTE_UTIL_H

#include <cstdint>
#include <cstring>


namespace Common::ByteUtil {
	inline std::uint16_t LoadBigU16(const std::uint8_t *p) {
		return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
	}

	inline std::uint32_t LoadBigU32(const std::uint8_t *p) {
		return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
			   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
	}

	inline std::uint16_t LoadLittleU16(const std::uint8_t *p) {
		return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
	}

	inline std::uint32_t LoadLittleU32(const std::uint8_t *p) {
		return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
			   (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
	}

	inline std::uint64_t LoadLittleU64(const std::uint8_t *p) {
		return static_cast<std::uint64_t>(LoadLittleU32(p)) | (static_cast<std::uint64_t>(LoadLittleU32(p + 4)) << 32);
	}

	inline void StoreBigU16(std::uint8_t *p, std::uint16_t v) {
		p[0] = static_cast<std::uint8_t>(v >> 8);
		p[1] = static_cast<std::uint8_t>(v);
	}

	inline void StoreBigU32(std::uint8_t *p, std::uint32_t v) {
		p[0] = static_cast<std::uint8_t>(v >> 24);
		p[1] = static_cast<std::uint8_t>(v >> 16);
		p[2] = static_cast<std::uint8_t>(v >> 8);
		p[3] = static_cast<std::uint8_t>(v);
	}

	inline void StoreLittleU16(std::uint8_t *p, std::uint16_t v) {
		p[0] = static_cast<std::uint8_t>(v);
		p[1] = static_cast<std::uint8_t>(v >> 8);
	}

	inline void StoreLittleU32(std::uint8_t *p, std::uint32_t v) {
		p[0] = static_cast<std::uint8_t>(v);
		p[1] = static_cast<std::uint8_t>(v >> 8);
		p[2] = static_cast<std::uint8_t>(v >> 16);
		p[3] = static_cast<std::uint8_t>(v >> 24);
	}
}  // namespace Common::ByteUtil

#endif	// COMMON_BYTE_UTIL_H
