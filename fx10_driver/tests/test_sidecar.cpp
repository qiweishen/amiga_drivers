#include "../include/timestamp_sidecar.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

using fx10::kFlagDeviceTsValid;
using fx10::kFlagGapBefore;
using fx10::kFlagPaddedZero;
using fx10::makeSidecarHeader;
using fx10::SidecarHeader;
using fx10::SidecarRecord;
using fx10::sidecarHeaderValid;

// The binary layout is a stable external format; lock every offset down so an
// accidental member reorder or type change fails loudly here.
static_assert(sizeof(SidecarHeader) == 64);
static_assert(offsetof(SidecarHeader, version) == 8);
static_assert(offsetof(SidecarHeader, record_size) == 12);
static_assert(offsetof(SidecarHeader, tick_frequency_hz) == 16);
static_assert(offsetof(SidecarHeader, session_start_realtime_ns) == 24);
static_assert(offsetof(SidecarHeader, session_start_monotonic_ns) == 32);
static_assert(offsetof(SidecarHeader, segment_index) == 40);
static_assert(offsetof(SidecarHeader, reserved) == 44);

static_assert(sizeof(SidecarRecord) == 48);
static_assert(offsetof(SidecarRecord, block_id) == 0);
static_assert(offsetof(SidecarRecord, host_realtime_ns) == 8);
static_assert(offsetof(SidecarRecord, host_monotonic_ns) == 16);
static_assert(offsetof(SidecarRecord, device_timestamp_ticks) == 24);
static_assert(offsetof(SidecarRecord, global_line_index) == 32);
static_assert(offsetof(SidecarRecord, flags) == 40);
static_assert(offsetof(SidecarRecord, gap_len) == 44);

TEST(Sidecar, HeaderFactoryAndValidation) {
  const SidecarHeader header = makeSidecarHeader(1000000000ull, 111, 222, 3);
  EXPECT_TRUE(sidecarHeaderValid(header));
  EXPECT_EQ(std::memcmp(header.magic, "FX10TS01", 8), 0);
  EXPECT_EQ(header.version, 1u);
  EXPECT_EQ(header.record_size, 48u);
  EXPECT_EQ(header.tick_frequency_hz, 1000000000ull);
  EXPECT_EQ(header.session_start_realtime_ns, 111u);
  EXPECT_EQ(header.session_start_monotonic_ns, 222u);
  EXPECT_EQ(header.segment_index, 3u);
  for (unsigned char byte : header.reserved) EXPECT_EQ(byte, 0);

  SidecarHeader corrupt = header;
  corrupt.magic[0] = 'X';
  EXPECT_FALSE(sidecarHeaderValid(corrupt));
  corrupt = header;
  corrupt.record_size = 40;
  EXPECT_FALSE(sidecarHeaderValid(corrupt));
}

TEST(Sidecar, RecordRoundTripThroughBytes) {
  SidecarRecord record{};
  record.block_id = 0x0102030405060708ull;
  record.host_realtime_ns = 1753000000123456789ull;
  record.host_monotonic_ns = 987654321ull;
  record.device_timestamp_ticks = 42ull;
  record.global_line_index = 1234567ull;
  record.flags = kFlagGapBefore | kFlagDeviceTsValid | kFlagPaddedZero;
  record.gap_len = 17;

  unsigned char buffer[sizeof(SidecarRecord)];
  std::memcpy(buffer, &record, sizeof(record));
  SidecarRecord decoded{};
  std::memcpy(&decoded, buffer, sizeof(decoded));

  EXPECT_EQ(decoded.block_id, record.block_id);
  EXPECT_EQ(decoded.host_realtime_ns, record.host_realtime_ns);
  EXPECT_EQ(decoded.host_monotonic_ns, record.host_monotonic_ns);
  EXPECT_EQ(decoded.device_timestamp_ticks, record.device_timestamp_ticks);
  EXPECT_EQ(decoded.global_line_index, record.global_line_index);
  EXPECT_EQ(decoded.flags, record.flags);
  EXPECT_EQ(decoded.gap_len, record.gap_len);
}

TEST(Sidecar, FlagBitsAreDistinct) {
  EXPECT_EQ(kFlagGapBefore, 1u);
  EXPECT_EQ(kFlagDeviceTsValid, 2u);
  EXPECT_EQ(kFlagPaddedZero, 4u);
}
