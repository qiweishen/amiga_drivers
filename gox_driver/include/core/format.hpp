#pragma once

// On-disk format of raw capture segment files ("jai-raw-seg", version 1.0).
//
// A segment file is:
//   FileHeader (512 bytes)  |  zero padding  |  FrameRecord  |  FrameRecord  |  ...
// where each FrameRecord is:
//   FrameHeader (96 bytes)  |  payload (payload_size bytes)  |  zero padding
// Every record starts at a multiple of record_align (mmap/O_DIRECT friendly):
// the first record at align_up(file_header_size, record_align), each next one
// at align_up(previous_record_end, record_align). record_align == 1 disables
// alignment (records are packed back to back right after the file header).
//
// All integers are little-endian (the only supported target is x86_64; the
// byte_order_mark field lets readers verify this assumption). Structs below
// are laid out with natural alignment and contain no implicit padding; this
// is locked down with static_asserts and golden-byte unit tests. Any layout
// change requires a version bump here and in scripts/inspect_raw.py and
// docs/FORMAT.md, which mirror this layout.

#include <cstddef>
#include <cstdint>

namespace jai::format {

	inline constexpr char kFileMagic[8] = { 'J', 'A', 'I', 'R', 'A', 'W', 'S', 'G' };
	inline constexpr uint32_t kFrameMagic = 0x4D415246u;  // little-endian bytes: "FRAM"
	inline constexpr uint32_t kByteOrderMark = 0x0A0B0C0Du;
	inline constexpr uint16_t kVersionMajor = 1;
	inline constexpr uint16_t kVersionMinor = 0;
	inline constexpr uint32_t kFileHeaderSize = 512;
	inline constexpr uint32_t kFrameHeaderSize = 96;
	inline constexpr uint32_t kDefaultRecordAlign = 4096;

	// FileHeader.segment_flags bits
	inline constexpr uint32_t kSegFlagChunkData = 1u << 0;	 // reserved, always 0 in v1
	inline constexpr uint32_t kSegFlagPayloadCrc = 1u << 1;	 // payload_crc32c fields are populated

	// FrameHeader.status_flags bits
	inline constexpr uint32_t kFrameFlagIncomplete = 1u << 0;		// missing packets; payload truncated
	inline constexpr uint32_t kFrameFlagResultNotOk = 1u << 1;		// other non-OK buffer result
	inline constexpr uint32_t kFrameFlagBlockIdGap = 1u << 2;		// gap vs previous frame's block_id
	inline constexpr uint32_t kFrameFlagDeviceTsSuspect = 1u << 3;	// zero or non-monotonic device ts
	inline constexpr uint32_t kFrameFlagChunkData = 1u << 4;		// reserved, always 0 in v1

	struct FileHeader {
		char file_magic[8];											// "JAIRAWSG"
		uint32_t file_header_size;									// = 512; readers must skip this many bytes
		uint16_t version_major;										// = 1
		uint16_t version_minor;										// = 0
		uint32_t byte_order_mark;									// = 0x0A0B0C0D read back as little-endian
		uint32_t segment_index;										// 1-based, matches file name seg_NNNNN.raw
		uint64_t created_realtime_ns;								// host CLOCK_REALTIME at segment creation
		uint8_t session_uuid[16];									// RFC 4122 binary
		char camera_id[64];											// logical camera name, NUL padded
		char camera_serial[64];										// device serial number, NUL padded
		uint32_t frame_header_size;									// = 96; readers must use this value
		uint32_t record_align;										// record start alignment; 1 = none
		uint32_t segment_flags;										// kSegFlag*
		uint8_t reserved[320];										// zero
		uint32_t header_crc32c;										// CRC-32C over bytes [0, 508)
	};

	struct FrameHeader {
		uint32_t frame_magic;		 // "FRAM"; resync anchor for crash recovery
		uint32_t header_size;		 // = 96; readers skip unknown trailing fields if larger
		uint64_t block_id;			 // GVSP BlockID (64-bit extended ID)
		uint64_t device_ts_ns;		 // camera timestamp; PTP (TAI) nanoseconds when synced
		uint64_t host_realtime_ns;	 // host CLOCK_REALTIME right after RetrieveBuffer
		uint64_t host_monotonic_ns;	 // host CLOCK_MONOTONIC, same instant
		uint32_t pixel_format;		 // GenICam PFNC 32-bit code
		uint32_t width;
		uint32_t height;
		uint32_t offset_x;
		uint32_t offset_y;
		uint32_t status_flags;	  // kFrameFlag*
		uint64_t payload_size;	  // bytes actually written after this header
		uint64_t frame_seq;		  // per-session monotonic write index, from 0
		uint32_t payload_crc32c;  // 0 unless kSegFlagPayloadCrc
		uint32_t reserved0;
		uint32_t reserved1;
		uint32_t header_crc32c;	  // CRC-32C over bytes [0, 92)
	};

	static_assert(sizeof(FileHeader) == kFileHeaderSize, "FileHeader must be exactly 512 bytes");
	static_assert(sizeof(FrameHeader) == kFrameHeaderSize, "FrameHeader must be exactly 96 bytes");

	static_assert(offsetof(FileHeader, file_header_size) == 8);
	static_assert(offsetof(FileHeader, version_major) == 12);
	static_assert(offsetof(FileHeader, version_minor) == 14);
	static_assert(offsetof(FileHeader, byte_order_mark) == 16);
	static_assert(offsetof(FileHeader, segment_index) == 20);
	static_assert(offsetof(FileHeader, created_realtime_ns) == 24);
	static_assert(offsetof(FileHeader, session_uuid) == 32);
	static_assert(offsetof(FileHeader, camera_id) == 48);
	static_assert(offsetof(FileHeader, camera_serial) == 112);
	static_assert(offsetof(FileHeader, frame_header_size) == 176);
	static_assert(offsetof(FileHeader, record_align) == 180);
	static_assert(offsetof(FileHeader, segment_flags) == 184);
	static_assert(offsetof(FileHeader, reserved) == 188);
	static_assert(offsetof(FileHeader, header_crc32c) == 508);

	static_assert(offsetof(FrameHeader, header_size) == 4);
	static_assert(offsetof(FrameHeader, block_id) == 8);
	static_assert(offsetof(FrameHeader, device_ts_ns) == 16);
	static_assert(offsetof(FrameHeader, host_realtime_ns) == 24);
	static_assert(offsetof(FrameHeader, host_monotonic_ns) == 32);
	static_assert(offsetof(FrameHeader, pixel_format) == 40);
	static_assert(offsetof(FrameHeader, width) == 44);
	static_assert(offsetof(FrameHeader, height) == 48);
	static_assert(offsetof(FrameHeader, offset_x) == 52);
	static_assert(offsetof(FrameHeader, offset_y) == 56);
	static_assert(offsetof(FrameHeader, status_flags) == 60);
	static_assert(offsetof(FrameHeader, payload_size) == 64);
	static_assert(offsetof(FrameHeader, frame_seq) == 72);
	static_assert(offsetof(FrameHeader, payload_crc32c) == 80);
	static_assert(offsetof(FrameHeader, header_crc32c) == 92);

	// CRC-32C (Castagnoli, polynomial 0x1EDC6F41, reflected). Uses the SSE4.2
	// hardware instruction when available, table-based fallback otherwise.
	// Standard test vector: crc32c("123456789", 9) == 0xE3069283.
	uint32_t crc32c(const void *data, size_t len, uint32_t seed = 0);

	constexpr uint64_t align_up(uint64_t value, uint64_t align) {
		return align <= 1 ? value : (value + align - 1) / align * align;
	}

	// Fills every field except header_crc32c, then computes it. camera_id and
	// camera_serial are truncated to 63 chars + NUL if longer.
	FileHeader make_file_header(uint32_t segment_index, uint64_t created_realtime_ns, const uint8_t session_uuid[16],
								const char *camera_id, const char *camera_serial, uint32_t record_align, uint32_t segment_flags);

	// Computes and stores header_crc32c over the first 92 bytes.
	void seal_frame_header(FrameHeader &header);

	// True if the stored header_crc32c matches the first 92 bytes.
	bool verify_frame_header(const FrameHeader &header);

	// True if the stored header_crc32c matches the first 508 bytes.
	bool verify_file_header(const FileHeader &header);

}  // namespace jai::format
