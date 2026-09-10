#pragma once

#include <cstdint>

#include "app_config.h"


// SDK-free sizing. Capture uses PlanRecordingBuffers to separate receive slack
// from disk-stall buffering under one payload-memory limit. PlanBufferPool is
// retained for callers that size a standalone SDK pool.

namespace fx10 {
    struct BufferPoolPlan {
        std::uint32_t count = 0; // buffers to allocate
        std::uint64_t bytes = 0; // count * payload_size
        bool clamped_by_memory = false; // max_buffer_memory_mb reduced the count
        bool clamped_by_stream = false; // GetQueuedBufferMaximum reduced the count
        double achievable_stall_s = 0.0; // stall absorption the final count provides
    };

    // expected_fps: configured frame rate (freerun) or expected trigger rate
    // stream_queue_max: PvStream::GetQueuedBufferMaximum(); 0 = no stream limit known
    BufferPoolPlan PlanBufferPool(const NetworkConfig &network, double expected_fps, std::uint64_t payload_size,
                                  std::uint32_t stream_queue_max);

    struct RecordingBufferPlan {
        std::uint32_t sdk_buffers = 0;
        std::uint32_t queue_frames = 0;
        std::uint64_t sdk_bytes = 0;
        std::uint64_t application_bytes = 0; // queue + producer + consumer, all holding SDK payload bytes
        std::uint64_t canonical_bytes = 0; // one unpacked frame, reused by the writer
        bool clamped_by_memory = false;
        bool clamped_by_stream = false;
        double achievable_stall_s = 0.0; // application queue only, no double counting SDK slack
    };

    // Shares max_buffer_memory_mb across receive buffers, the owned application
    // pool and the unpack scratch space. An empty plan means the budget is too small.
    RecordingBufferPlan PlanRecordingBuffers(const NetworkConfig &network, double expected_fps,
                                             std::uint64_t payload_size, std::uint64_t canonical_bytes,
                                             std::uint32_t stream_queue_max);
} // namespace fx10
