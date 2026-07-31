#include "../include/buffer_sizing.hpp"

#include <gtest/gtest.h>

using fx10::NetworkConfig;
using fx10::planBufferPool;

namespace {
constexpr std::uint64_t kPayload = 1024ull * 224 * 2;  // 448 KiB FX10e default frame
}

TEST(BufferSizing, AutoFormulaAtDefaultRate) {
  NetworkConfig net;  // buffer_count 0 = auto, stall 2.0 s, cap 512 MB
  const auto plan = planBufferPool(net, 50.0, kPayload, 0);
  EXPECT_EQ(plan.count, 116u);  // ceil(50*2)+16
  EXPECT_FALSE(plan.clamped_by_memory);
  EXPECT_DOUBLE_EQ(plan.achievable_stall_s, 2.0);
}

TEST(BufferSizing, MemoryCapClamps) {
  NetworkConfig net;
  const auto plan = planBufferPool(net, 327.0, kPayload, 0);
  // ceil(327*2)+16 = 670 buffers = 293 MiB < 512 MiB -> no clamp
  EXPECT_EQ(plan.count, 670u);
  EXPECT_FALSE(plan.clamped_by_memory);

  net.max_buffer_memory_mb = 64;  // 64 MiB / 448 KiB = 146 buffers
  const auto clamped = planBufferPool(net, 327.0, kPayload, 0);
  EXPECT_TRUE(clamped.clamped_by_memory);
  EXPECT_EQ(clamped.count, 146u);
  EXPECT_LT(clamped.achievable_stall_s, 0.5);
}

TEST(BufferSizing, StreamQueueMaxClamps) {
  NetworkConfig net;
  const auto plan = planBufferPool(net, 327.0, kPayload, 128);
  EXPECT_TRUE(plan.clamped_by_stream);
  EXPECT_EQ(plan.count, 128u);
}

TEST(BufferSizing, ExplicitCountBypassesAuto) {
  NetworkConfig net;
  net.buffer_count = 42;
  const auto plan = planBufferPool(net, 327.0, kPayload, 0);
  EXPECT_EQ(plan.count, 42u);
}

TEST(BufferSizing, FloorAndDegenerateInputs) {
  NetworkConfig net;
  net.buffer_count = 2;  // below floor
  EXPECT_EQ(planBufferPool(net, 50.0, kPayload, 0).count, 8u);
  EXPECT_EQ(planBufferPool(net, 50.0, 0, 0).count, 0u);  // zero payload = no plan
  net.buffer_count = 0;
  EXPECT_GT(planBufferPool(net, 0.0, kPayload, 0).count, 0u);  // fps 0 falls back
}

TEST(BufferSizing, HardCapsWinOverFloor) {
  NetworkConfig net;
  const auto plan = planBufferPool(net, 50.0, kPayload, 4);  // stream accepts only 4
  EXPECT_EQ(plan.count, 4u);  // floor of 8 must NOT override the stream limit
  EXPECT_TRUE(plan.clamped_by_stream);
}
