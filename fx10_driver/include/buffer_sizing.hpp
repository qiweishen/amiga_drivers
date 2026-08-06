#pragma once

#include <cstdint>

#include "app_config.hpp"


// Pure buffer-pool sizing (unit-tested in fx10_core; the eBUS transport only applies the result)
// Policy: absorb `stall_budget_s` of disk stall at the expected frame rate, clamped by the memory cap and the stream's queue limit

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
    BufferPoolPlan planBufferPool(const NetworkConfig &network, double expected_fps, std::uint64_t payload_size,
                                  std::uint32_t stream_queue_max);
} // namespace fx10
