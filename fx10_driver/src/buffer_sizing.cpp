#include "buffer_sizing.h"

#include <algorithm>
#include <cmath>
#include <limits>


namespace fx10 {
    namespace {
        constexpr std::uint32_t kAutoMargin = 16; // slack on top of the stall budget
        constexpr std::uint32_t kMinBuffers = 8;
    } // namespace

    RecordingBufferPlan PlanRecordingBuffers(const NetworkConfig &network, double expected_fps,
                                             std::uint64_t payload_size, std::uint64_t canonical_bytes,
                                             std::uint32_t stream_queue_max) {
        RecordingBufferPlan plan;
        if (payload_size == 0 || canonical_bytes == 0 || network.max_buffer_memory_mb <= 0 ||
            !std::isfinite(network.stall_budget_s) || network.stall_budget_s < 0.0) return plan;
        const auto cap = static_cast<std::uint64_t>(network.max_buffer_memory_mb) * 1024ull * 1024ull;
        if (canonical_bytes >= cap) return plan;
        const auto slots = (cap - canonical_bytes) / payload_size;
        // At least one SDK buffer, one queued item, producer and consumer.
        if (slots < 4) return plan;
        const double fps = std::isfinite(expected_fps) && expected_fps > 0.0 ? expected_fps : 1.0;
        auto sdk = network.buffer_count > 0
            ? std::max<std::uint64_t>(static_cast<std::uint64_t>(network.buffer_count), kMinBuffers)
            : static_cast<std::uint64_t>(kAutoMargin);
        if (stream_queue_max != 0 && sdk > stream_queue_max) {
            sdk = stream_queue_max;
            plan.clamped_by_stream = true;
        }
        if (sdk > slots - 3) {
            sdk = slots - 3;
            plan.clamped_by_memory = true;
        }
        const auto max_queue = std::min<std::uint64_t>(slots - sdk - 2,
                                                       std::numeric_limits<std::uint32_t>::max() - 2ull);
        const double desired = std::max(1.0, std::ceil(fps * network.stall_budget_s));
        const auto queue = desired > static_cast<double>(max_queue)
            ? max_queue : static_cast<std::uint64_t>(desired);
        plan.clamped_by_memory = plan.clamped_by_memory || desired > static_cast<double>(max_queue);
        plan.sdk_buffers = static_cast<std::uint32_t>(sdk);
        plan.queue_frames = static_cast<std::uint32_t>(queue);
        plan.sdk_bytes = sdk * payload_size;
        plan.application_bytes = (queue + 2) * payload_size;
        plan.canonical_bytes = canonical_bytes;
        plan.achievable_stall_s = static_cast<double>(queue) / fps;
        return plan;
    }


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
