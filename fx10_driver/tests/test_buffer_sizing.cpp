#include "../include/buffer_sizing.h"

#include <doctest/doctest.h>

using fx10::NetworkConfig;
using fx10::PlanBufferPool;

namespace {
    constexpr std::uint64_t kPayload = 1024ull * 224 * 2; // 448 KiB FX10e default frame
}

TEST_CASE("BufferSizing: AutoFormulaAtDefaultRate") {
    NetworkConfig net; // buffer_count 0 = auto, stall 2.0 s, cap 512 MB
    const auto plan = PlanBufferPool(net, 50.0, kPayload, 0);
    CHECK(plan.count == 116u); // ceil(50*2)+16
    CHECK_FALSE(plan.clamped_by_memory);
    CHECK(plan.achievable_stall_s == doctest::Approx(2.0));
}

TEST_CASE("BufferSizing: MemoryCapClamps") {
    NetworkConfig net;
    const auto plan = PlanBufferPool(net, 327.0, kPayload, 0);
    // ceil(327*2)+16 = 670 buffers = 293 MiB < 512 MiB -> no clamp
    CHECK(plan.count == 670u);
    CHECK_FALSE(plan.clamped_by_memory);

    net.max_buffer_memory_mb = 64; // 64 MiB / 448 KiB = 146 buffers
    const auto clamped = PlanBufferPool(net, 327.0, kPayload, 0);
    CHECK(clamped.clamped_by_memory);
    CHECK(clamped.count == 146u);
    CHECK(clamped.achievable_stall_s < 0.5);
}

TEST_CASE("BufferSizing: StreamQueueMaxClamps") {
    NetworkConfig net;
    const auto plan = PlanBufferPool(net, 327.0, kPayload, 128);
    CHECK(plan.clamped_by_stream);
    CHECK(plan.count == 128u);
}

TEST_CASE("BufferSizing: ExplicitCountBypassesAuto") {
    NetworkConfig net;
    net.buffer_count = 42;
    const auto plan = PlanBufferPool(net, 327.0, kPayload, 0);
    CHECK(plan.count == 42u);
}

TEST_CASE("BufferSizing: FloorAndDegenerateInputs") {
    NetworkConfig net;
    net.buffer_count = 2; // below floor
    CHECK(PlanBufferPool(net, 50.0, kPayload, 0).count == 8u);
    CHECK(PlanBufferPool(net, 50.0, 0, 0).count == 0u); // zero payload = no plan
    net.buffer_count = 0;
    CHECK(PlanBufferPool(net, 0.0, kPayload, 0).count > 0u); // fps 0 falls back
}

TEST_CASE("BufferSizing: HardCapsWinOverFloor") {
    NetworkConfig net;
    const auto plan = PlanBufferPool(net, 50.0, kPayload, 4); // stream accepts only 4
    CHECK(plan.count == 4u); // floor of 8 must NOT override the stream limit
    CHECK(plan.clamped_by_stream);
}
