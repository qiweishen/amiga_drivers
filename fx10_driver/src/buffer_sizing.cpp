#include "buffer_sizing.h"

#include <algorithm>
#include <cmath>


namespace fx10 {
    namespace {
        constexpr std::uint32_t kAutoMargin = 16; // slack on top of the stall budget
        constexpr std::uint32_t kMinBuffers = 8;
    } // namespace


    BufferPoolPlan PlanBufferPool(const NetworkConfig &network, double expected_fps,
                                  std::uint64_t payload_size, std::uint32_t stream_queue_max) {
        BufferPoolPlan plan;
        if (payload_size == 0) {
            return plan;
        }

        std::uint64_t count;
        if (network.buffer_count > 0) {
            count = static_cast<std::uint64_t>(network.buffer_count);
        } else {
            const double fps = expected_fps > 0.0 ? expected_fps : 1.0;
            count = static_cast<std::uint64_t>(std::ceil(fps * network.stall_budget_s)) + kAutoMargin;
        }
        // Desired floor first; the memory and stream-queue limits below are HARD caps
        // and must win over it (a stream that only accepts 4 buffers gets 4)
        count = std::max<std::uint64_t>(count, kMinBuffers);

        const std::uint64_t memory_cap = static_cast<std::uint64_t>(network.max_buffer_memory_mb) * 1024ull * 1024ull;
        const std::uint64_t max_by_memory = std::max<std::uint64_t>(memory_cap / payload_size, 1);
        if (count > max_by_memory) {
            count = max_by_memory;
            plan.clamped_by_memory = true;
        }
        if (stream_queue_max > 0 && count > stream_queue_max) {
            count = stream_queue_max;
            plan.clamped_by_stream = true;
        }

        plan.count = static_cast<std::uint32_t>(count);
        plan.bytes = count * payload_size;
        const double fps = expected_fps > 0.0 ? expected_fps : 1.0;
        // The margin only exists in the auto-sized pool; never let the estimate go non-monotonic
        const bool auto_sized = network.buffer_count <= 0;
        const std::uint64_t usable = auto_sized ? std::max<std::uint64_t>(count, kAutoMargin + 1) - kAutoMargin : count;
        plan.achievable_stall_s = static_cast<double>(usable) / fps;
        return plan;
    }
} // namespace fx10
