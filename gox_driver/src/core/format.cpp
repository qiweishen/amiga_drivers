#include "core/format.hpp"

#include <cstring>

#if defined(__x86_64__)
	#include <nmmintrin.h>
#endif

namespace jai::format {

	namespace {

		// Table-based CRC-32C, slice-by-1. Fast enough for headers; the hardware
		// path below covers bulk payload checksumming.
		struct Crc32cTable {
			uint32_t t[256];
			Crc32cTable() {
				for (uint32_t i = 0; i < 256; ++i) {
					uint32_t crc = i;
					for (int j = 0; j < 8; ++j) {
						crc = (crc >> 1) ^ (0x82F63B78u & (~(crc & 1) + 1));
					}
					t[i] = crc;
				}
			}
		};

		uint32_t crc32c_sw(const uint8_t *p, size_t len, uint32_t crc) {
			static const Crc32cTable table;
			while (len--) {
				crc = table.t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
			}
			return crc;
		}

#if defined(__x86_64__)
		__attribute__((target("sse4.2"))) uint32_t crc32c_hw(const uint8_t *p, size_t len, uint32_t crc) {
			uint64_t crc64 = crc;
			while (len >= 8) {
				uint64_t v;
				std::memcpy(&v, p, 8);
				crc64 = _mm_crc32_u64(crc64, v);
				p += 8;
				len -= 8;
			}
			uint32_t crc32 = static_cast<uint32_t>(crc64);
			while (len--) {
				crc32 = _mm_crc32_u8(crc32, *p++);
			}
			return crc32;
		}

		bool have_sse42() {
			static const bool ok = __builtin_cpu_supports("sse4.2");
			return ok;
		}
#endif

	}  // namespace

	uint32_t crc32c(const void *data, size_t len, uint32_t seed) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		uint32_t crc = ~seed;
#if defined(__x86_64__)
		if (have_sse42()) {
			return ~crc32c_hw(p, len, crc);
		}
#endif
		return ~crc32c_sw(p, len, crc);
	}

	FileHeader make_file_header(uint32_t segment_index, uint64_t created_realtime_ns, const uint8_t session_uuid[16],
								const char *camera_id, const char *camera_serial, uint32_t record_align, uint32_t segment_flags) {
		FileHeader h{};
		std::memcpy(h.file_magic, kFileMagic, sizeof(kFileMagic));
		h.file_header_size = kFileHeaderSize;
		h.version_major = kVersionMajor;
		h.version_minor = kVersionMinor;
		h.byte_order_mark = kByteOrderMark;
		h.segment_index = segment_index;
		h.created_realtime_ns = created_realtime_ns;
		std::memcpy(h.session_uuid, session_uuid, 16);
		std::strncpy(h.camera_id, camera_id ? camera_id : "", sizeof(h.camera_id) - 1);
		std::strncpy(h.camera_serial, camera_serial ? camera_serial : "", sizeof(h.camera_serial) - 1);
		h.frame_header_size = kFrameHeaderSize;
		h.record_align = record_align;
		h.segment_flags = segment_flags;
		h.header_crc32c = crc32c(&h, offsetof(FileHeader, header_crc32c));
		return h;
	}

	void seal_frame_header(FrameHeader &header) {
		header.header_crc32c = crc32c(&header, offsetof(FrameHeader, header_crc32c));
	}

	bool verify_frame_header(const FrameHeader &header) {
		return header.header_crc32c == crc32c(&header, offsetof(FrameHeader, header_crc32c));
	}

	bool verify_file_header(const FileHeader &header) {
		return header.header_crc32c == crc32c(&header, offsetof(FileHeader, header_crc32c));
	}

}  // namespace jai::format
