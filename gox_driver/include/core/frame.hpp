#pragma once

// FrameMeta and FrameChunk are the only data types that cross the boundary
// between the SDK-dependent acquisition layer (src/ebus) and the SDK-free
// recording layer (src/core). Nothing in src/core may include Pv*.h.

#include <cstdint>
#include <memory>

namespace jai {

	struct FrameMeta {
		uint32_t camera_index = 0;		// index into config cameras[]
		uint64_t block_id = 0;			// GVSP BlockID
		uint64_t device_ts_ns = 0;		// camera timestamp (PTP/TAI ns when synced)
		uint64_t host_realtime_ns = 0;	// sampled right after RetrieveBuffer returns
		uint64_t host_monotonic_ns = 0;
		uint32_t pixel_format = 0;		// GenICam PFNC code
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t offset_x = 0;
		uint32_t offset_y = 0;
		uint32_t status_flags = 0;	 // jai::format::kFrameFlag*
		uint64_t payload_size = 0;	 // valid bytes in FrameChunk::data
		uint64_t expected_size = 0;	 // device PayloadSize (differs when incomplete)
	};

	// One captured frame: metadata + an owned copy of the raw payload bytes.
	struct FrameChunk {
		FrameMeta meta;
		std::unique_ptr<uint8_t[]> data;
		size_t capacity = 0;

		explicit FrameChunk(size_t cap) : data(new uint8_t[cap]), capacity(cap) {}
	};

	using FrameChunkPtr = std::unique_ptr<FrameChunk>;

}  // namespace jai
