#pragma once

// FrameMeta and FrameChunk are the only data types that cross the boundary
// between the SDK-dependent acquisition layer (src/ebus) and the SDK-free
// recording layer. Nothing outside src/ebus may include Pv*.h.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace gox {
    struct FrameMeta {
        uint64_t block_id = 0; // GVSP BlockID
        uint64_t device_ts_ns = 0; // legacy field name: unscaled SDK timestamp; unit/epoch unverified
        uint64_t host_realtime_ns = 0; // sampled right after RetrieveBuffer returns
        uint64_t host_monotonic_ns = 0;
        uint32_t pixel_format = 0; // GenICam PFNC code
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t offset_x = 0;
        uint32_t offset_y = 0;
        uint32_t status_flags = 0; // gox::format::kFrameFlag*
        uint64_t payload_size = 0; // valid bytes in FrameChunk::data
        // Supplementary SDK layout/error metadata stored in the JSONL index.
        // The version-1 binary header cannot reconstruct these if that index is lost.
        uint32_t payload_type = 0;
        uint32_t padding_x = 0;
        uint32_t padding_y = 0;
        uint32_t chunk_count = 0;
        uint32_t operation_result = 0;
    };

    // One captured frame: metadata + an owned copy of the raw payload bytes.
    struct FrameChunk {
        FrameMeta meta;
        std::unique_ptr<uint8_t[]> data;
        size_t capacity = 0;

        explicit FrameChunk(size_t cap) : data(new uint8_t[cap]), capacity(cap) {
        }
    };

    using FrameChunkPtr = std::unique_ptr<FrameChunk>;
} // namespace gox
