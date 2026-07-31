#include "../include/envi_recorder.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using ::testing::HasSubstr;
namespace fs = std::filesystem;

using fx10::Counters;
using fx10::createSessionDir;
using fx10::EnviDataType;
using fx10::EnviRecorder;
using fx10::FrameView;
using fx10::GapPolicy;
using fx10::RecorderError;
using fx10::RecorderInit;
using fx10::RecordingConfig;
using fx10::SidecarHeader;
using fx10::SidecarRecord;

namespace {

constexpr std::uint32_t kSamples = 4;
constexpr std::uint32_t kBands = 3;
constexpr std::size_t kLineBytes = kSamples * kBands * 2;

std::atomic<int> g_dir_counter{0};

fs::path makeTempDir() {
  const fs::path dir = fs::path(::testing::TempDir()) /
                       ("fx10_rec_" + std::to_string(::getpid()) + "_" +
                        std::to_string(g_dir_counter++));
  fs::create_directories(dir);
  return dir;
}

RecordingConfig makeConfig(const fs::path& dir) {
  RecordingConfig config;
  config.output_dir = dir.string();
  config.base_name = "t";
  config.rotation.max_lines = 0;
  config.rotation.max_megabytes = 0;
  config.on_gap = GapPolicy::kRecord;
  return config;
}

RecorderInit makeInit() {
  RecorderInit init;
  init.samples = kSamples;
  init.bands = kBands;
  init.bytes_per_pixel = 2;
  init.data_type = EnviDataType::kUint16;
  init.wavelengths.nm = {400.0, 700.0, 1000.0};
  init.wavelengths.source_tag = "list";
  init.description = "unit test";
  init.tick_frequency_hz = 1000000000ull;
  return init;
}

std::vector<std::uint8_t> makeLine(std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(kLineBytes);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(seed + i);
  }
  return bytes;
}

FrameView makeFrame(const std::vector<std::uint8_t>& bytes, std::uint64_t block_id) {
  FrameView frame;
  frame.data = bytes.data();
  frame.size = bytes.size();
  frame.width = kSamples;
  frame.height = kBands;
  frame.bytes_per_pixel = 2;
  frame.block_id = block_id;
  frame.device_timestamp_ticks = 500 + block_id;
  frame.host_realtime_ns = 1000 + static_cast<std::int64_t>(block_id);
  frame.host_monotonic_ns = 2000 + static_cast<std::int64_t>(block_id);
  return frame;
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
}

std::vector<SidecarRecord> readSidecarRecords(const fs::path& path, SidecarHeader* header_out) {
  const auto bytes = readFile(path);
  EXPECT_GE(bytes.size(), sizeof(SidecarHeader));
  SidecarHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (header_out) *header_out = header;
  const std::size_t n = (bytes.size() - sizeof(SidecarHeader)) / sizeof(SidecarRecord);
  std::vector<SidecarRecord> records(n);
  std::memcpy(records.data(), bytes.data() + sizeof(SidecarHeader), n * sizeof(SidecarRecord));
  return records;
}

bool anyPartFiles(const fs::path& dir) {
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    if (entry.path().string().find(".part") != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST(EnviRecorder, ByteExactBilSidecarHeaderAndLedger) {
  const fs::path tmp = makeTempDir();
  Counters counters;
  EnviRecorder recorder(makeConfig(tmp), counters);
  recorder.start(makeInit());

  const auto line1 = makeLine(1);
  const auto line2 = makeLine(50);
  const auto line3 = makeLine(200);
  recorder.onFrame(makeFrame(line1, 10));
  recorder.onFrame(makeFrame(line2, 11));
  recorder.onFrame(makeFrame(line3, 12));
  recorder.stop();

  const fs::path dir = recorder.sessionDir();
  ASSERT_TRUE(fs::exists(dir / "segment_0001.bil"));
  ASSERT_TRUE(fs::exists(dir / "segment_0001.hdr"));
  ASSERT_TRUE(fs::exists(dir / "segment_0001.times"));
  EXPECT_FALSE(anyPartFiles(dir));

  // .bil is the byte-for-byte concatenation of the frame payloads.
  std::vector<std::uint8_t> expected;
  expected.insert(expected.end(), line1.begin(), line1.end());
  expected.insert(expected.end(), line2.begin(), line2.end());
  expected.insert(expected.end(), line3.begin(), line3.end());
  EXPECT_EQ(readFile(dir / "segment_0001.bil"), expected);

  SidecarHeader header{};
  const auto records = readSidecarRecords(dir / "segment_0001.times", &header);
  EXPECT_TRUE(fx10::sidecarHeaderValid(header));
  EXPECT_EQ(header.segment_index, 1u);
  EXPECT_EQ(header.tick_frequency_hz, 1000000000ull);
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].block_id, 10u);
  EXPECT_EQ(records[2].block_id, 12u);
  EXPECT_EQ(records[1].global_line_index, 1u);
  EXPECT_TRUE(records[0].flags & fx10::kFlagDeviceTsValid);
  EXPECT_EQ(records[0].gap_len, 0u);

  const auto hdr_bytes = readFile(dir / "segment_0001.hdr");
  const std::string hdr(hdr_bytes.begin(), hdr_bytes.end());
  EXPECT_THAT(hdr, HasSubstr("samples = 4"));
  EXPECT_THAT(hdr, HasSubstr("lines = 3"));
  EXPECT_THAT(hdr, HasSubstr("bands = 3"));
  EXPECT_THAT(hdr, HasSubstr("interleave = bil"));
  EXPECT_THAT(hdr, HasSubstr("wavelength source: list"));

  EXPECT_EQ(fx10::classify(counters), fx10::RunStatus::kClean);
  EXPECT_EQ(counters.frames_written, 3u);
  EXPECT_EQ(counters.segments_finalized, 1u);
  EXPECT_EQ(counters.bytes_written, 3 * kLineBytes);
}

TEST(EnviRecorder, RotationByMaxLines) {
  const fs::path tmp = makeTempDir();
  RecordingConfig config = makeConfig(tmp);
  config.rotation.max_lines = 2;
  Counters counters;
  EnviRecorder recorder(config, counters);
  recorder.start(makeInit());

  const auto line = makeLine(7);
  for (std::uint64_t id = 1; id <= 5; ++id) {
    recorder.onFrame(makeFrame(line, id));
  }
  recorder.stop();

  const fs::path dir = recorder.sessionDir();
  for (const char* name : {"segment_0001", "segment_0002", "segment_0003"}) {
    EXPECT_TRUE(fs::exists(dir / (std::string(name) + ".bil"))) << name;
    EXPECT_TRUE(fs::exists(dir / (std::string(name) + ".hdr"))) << name;
  }
  EXPECT_EQ(fs::file_size(dir / "segment_0001.bil"), 2 * kLineBytes);
  EXPECT_EQ(fs::file_size(dir / "segment_0003.bil"), 1 * kLineBytes);

  SidecarHeader header{};
  const auto records = readSidecarRecords(dir / "segment_0003.times", &header);
  EXPECT_EQ(header.segment_index, 3u);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].global_line_index, 4u);  // continuous across segments

  EXPECT_EQ(counters.segments_finalized, 3u);
}

TEST(EnviRecorder, GapRecordPolicyMarksNextRecord) {
  const fs::path tmp = makeTempDir();
  Counters counters;
  EnviRecorder recorder(makeConfig(tmp), counters);
  recorder.start(makeInit());

  const auto line = makeLine(3);
  recorder.onFrame(makeFrame(line, 1));
  recorder.onGap(2, 3);  // blocks 2,3,4 lost
  recorder.onFrame(makeFrame(line, 5));
  recorder.stop();

  const fs::path dir = recorder.sessionDir();
  EXPECT_EQ(fs::file_size(dir / "segment_0001.bil"), 2 * kLineBytes);
  const auto records = readSidecarRecords(dir / "segment_0001.times", nullptr);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_FALSE(records[0].flags & fx10::kFlagGapBefore);
  EXPECT_TRUE(records[1].flags & fx10::kFlagGapBefore);
  EXPECT_EQ(records[1].gap_len, 3u);
  EXPECT_EQ(counters.frames_missed_rx, 3u);
  EXPECT_EQ(counters.blockid_gap_events, 1u);
  EXPECT_EQ(fx10::classify(counters), fx10::RunStatus::kDegraded);
}

TEST(EnviRecorder, GapPadZeroPolicyWritesZeroLines) {
  const fs::path tmp = makeTempDir();
  RecordingConfig config = makeConfig(tmp);
  config.on_gap = GapPolicy::kPadZero;
  Counters counters;
  EnviRecorder recorder(config, counters);
  recorder.start(makeInit());

  const auto line_a = makeLine(1);
  const auto line_b = makeLine(99);
  recorder.onFrame(makeFrame(line_a, 1));
  recorder.onGap(2, 2);
  recorder.onFrame(makeFrame(line_b, 4));
  recorder.stop();

  const fs::path dir = recorder.sessionDir();
  const auto bil = readFile(dir / "segment_0001.bil");
  ASSERT_EQ(bil.size(), 4 * kLineBytes);
  const std::vector<std::uint8_t> zeros(kLineBytes, 0);
  EXPECT_TRUE(std::equal(bil.begin() + kLineBytes, bil.begin() + 2 * kLineBytes, zeros.begin()));
  EXPECT_TRUE(std::equal(bil.begin() + 2 * kLineBytes, bil.begin() + 3 * kLineBytes,
                         zeros.begin()));

  const auto records = readSidecarRecords(dir / "segment_0001.times", nullptr);
  ASSERT_EQ(records.size(), 4u);
  EXPECT_TRUE(records[1].flags & fx10::kFlagPaddedZero);
  EXPECT_TRUE(records[1].flags & fx10::kFlagGapBefore);
  EXPECT_EQ(records[1].gap_len, 2u);
  EXPECT_TRUE(records[2].flags & fx10::kFlagPaddedZero);
  EXPECT_FALSE(records[2].flags & fx10::kFlagGapBefore);
  EXPECT_FALSE(records[3].flags & fx10::kFlagPaddedZero);
  EXPECT_EQ(records[3].block_id, 4u);
  EXPECT_EQ(counters.gap_lines_padded, 2u);
  // bytes_written matches the .bil on disk, padded lines included.
  EXPECT_EQ(counters.bytes_written, 4 * kLineBytes);
  EXPECT_EQ(fx10::classify(counters), fx10::RunStatus::kDegraded);  // frames were lost on RX
  // .hdr line count includes the padded lines (cube geometry).
  const auto hdr_bytes = readFile(dir / "segment_0001.hdr");
  EXPECT_THAT(std::string(hdr_bytes.begin(), hdr_bytes.end()), HasSubstr("lines = 4"));
}

TEST(EnviRecorder, WriteFailureTruncatesFinalizesAndLatches) {
  const fs::path tmp = makeTempDir();
  Counters counters;
  EnviRecorder recorder(makeConfig(tmp), counters);

  int calls = 0;
  recorder.setWriteHookForTest([&calls](int fd, const void* buf, std::size_t n) -> ssize_t {
    // Call 1: sidecar file header. Calls 2+3: frame 1 data+record.
    // Call 4: frame 2 data -> simulated disk full.
    if (++calls == 4) {
      errno = ENOSPC;
      return -1;
    }
    return ::write(fd, buf, n);
  });

  recorder.start(makeInit());
  const auto line = makeLine(1);
  recorder.onFrame(makeFrame(line, 1));
  recorder.onFrame(makeFrame(line, 2));  // fails, latches
  EXPECT_TRUE(recorder.failed());
  EXPECT_THAT(recorder.errorMessage(), HasSubstr("write failed"));
  recorder.onFrame(makeFrame(line, 3));  // ignored after latch
  recorder.stop();

  const fs::path dir = recorder.sessionDir();
  // Finalized partial segment: exactly 1 complete line, valid .hdr, no .part files.
  ASSERT_TRUE(fs::exists(dir / "segment_0001.bil"));
  EXPECT_EQ(fs::file_size(dir / "segment_0001.bil"), 1 * kLineBytes);
  EXPECT_EQ(fs::file_size(dir / "segment_0001.times"),
            sizeof(SidecarHeader) + 1 * sizeof(SidecarRecord));
  const auto hdr_bytes = readFile(dir / "segment_0001.hdr");
  EXPECT_THAT(std::string(hdr_bytes.begin(), hdr_bytes.end()), HasSubstr("lines = 1"));
  EXPECT_FALSE(anyPartFiles(dir));

  EXPECT_GE(counters.write_errors, 1u);
  EXPECT_EQ(counters.frames_written, 1u);
  EXPECT_TRUE(recorder.failed());  // the run counts as FAILED, not merely degraded
}

TEST(EnviRecorder, SizeMismatchDropped) {
  const fs::path tmp = makeTempDir();
  Counters counters;
  EnviRecorder recorder(makeConfig(tmp), counters);
  recorder.start(makeInit());

  std::vector<std::uint8_t> short_line(kLineBytes - 2, 1);
  FrameView bad = makeFrame(short_line, 1);
  bad.size = short_line.size();
  recorder.onFrame(bad);

  const auto line = makeLine(2);
  recorder.onFrame(makeFrame(line, 2));
  recorder.stop();

  EXPECT_EQ(counters.size_mismatch_drops, 1u);
  EXPECT_EQ(counters.frames_written, 1u);
  const auto records = readSidecarRecords(recorder.sessionDir() / "segment_0001.times", nullptr);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].global_line_index, 0u);
}

TEST(EnviRecorder, ConsecutiveSizeMismatchAborts) {
  const fs::path tmp = makeTempDir();
  Counters counters;
  EnviRecorder recorder(makeConfig(tmp), counters);
  recorder.start(makeInit());

  std::vector<std::uint8_t> short_line(kLineBytes - 2, 1);
  for (int i = 0; i < 26 && !recorder.failed(); ++i) {
    FrameView bad = makeFrame(short_line, static_cast<std::uint64_t>(i + 1));
    bad.size = short_line.size();
    recorder.onFrame(bad);
  }
  EXPECT_TRUE(recorder.failed());
  EXPECT_THAT(recorder.errorMessage(), HasSubstr("consecutive"));
  EXPECT_EQ(counters.size_mismatch_drops, 26u);
  recorder.stop();
  EXPECT_TRUE(recorder.failed());
}

TEST(EnviRecorder, EmptyTrailingSegmentRemoved) {
  const fs::path tmp = makeTempDir();
  RecordingConfig config = makeConfig(tmp);
  config.rotation.max_lines = 1;
  Counters counters;
  EnviRecorder recorder(config, counters);
  recorder.start(makeInit());
  recorder.onFrame(makeFrame(makeLine(1), 1));  // finalizes seg 1, opens seg 2
  recorder.stop();                              // seg 2 has 0 lines -> removed

  const fs::path dir = recorder.sessionDir();
  EXPECT_TRUE(fs::exists(dir / "segment_0001.bil"));
  EXPECT_FALSE(fs::exists(dir / "segment_0002.bil"));
  EXPECT_FALSE(anyPartFiles(dir));
  EXPECT_EQ(counters.segments_finalized, 1u);
}

TEST(EnviRecorder, SessionDirNoClobber) {
  const fs::path tmp = makeTempDir();
  const fs::path first = createSessionDir(tmp, "x", "20260721T000000Z");
  const fs::path second = createSessionDir(tmp, "x", "20260721T000000Z");
  EXPECT_TRUE(fs::exists(first));
  EXPECT_TRUE(fs::exists(second));
  EXPECT_NE(first, second);
  EXPECT_EQ(second.filename().string(), first.filename().string() + "_1");
}

TEST(EnviRecorder, StartValidation) {
  const fs::path tmp = makeTempDir();
  Counters counters;

  {
    EnviRecorder recorder(makeConfig(tmp), counters);
    RecorderInit init = makeInit();
    init.samples = 0;
    EXPECT_THROW(recorder.start(init), RecorderError);
  }
  {
    EnviRecorder recorder(makeConfig(tmp), counters);
    RecorderInit init = makeInit();
    init.bytes_per_pixel = 3;
    EXPECT_THROW(recorder.start(init), RecorderError);
  }
  {
    EnviRecorder recorder(makeConfig(tmp), counters);
    RecorderInit init = makeInit();
    init.bytes_per_pixel = 1;  // mismatch: data_type still uint16
    EXPECT_THROW(recorder.start(init), RecorderError);
  }
  {
    EnviRecorder recorder(makeConfig(tmp), counters);
    RecorderInit init = makeInit();
    init.wavelengths.nm = {400.0};  // 1 value, 3 bands
    EXPECT_THROW(recorder.start(init), RecorderError);
  }
}
