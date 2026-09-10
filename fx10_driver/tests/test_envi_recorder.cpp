#include "../include/envi_recorder.h"

#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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

namespace {
    constexpr std::uint32_t kSamples = 4;
    constexpr std::uint32_t kBands = 3;
    constexpr std::size_t kLineBytes = kSamples * kBands * 2;

    std::atomic<int> g_dir_counter{0};

    fs::path MakeTempDir() {
        const fs::path dir = fs::path(std::filesystem::temp_directory_path().string()) /
                             ("fx10_rec_" + std::to_string(::getpid()) + "_" +
                              std::to_string(g_dir_counter++));
        fs::create_directories(dir);
        return dir;
    }

    RecordingConfig MakeConfig(const fs::path &dir) {
        RecordingConfig config;
        config.output_dir = dir.string();
        config.rotation.max_lines = 0;
        config.rotation.max_mb = 0;
        config.on_gap = GapPolicy::kRecord;
        return config;
    }

    RecorderInit MakeInit() {
        RecorderInit init;
        init.samples = kSamples;
        init.bands = kBands;
        init.bytes_per_pixel = 2;
        init.data_type = EnviDataType::kUint16;
        init.wavelengths.nm = {400.0, 700.0, 1000.0};
        init.wavelengths.source_tag = "list";
        init.description = "unit test";
        return init;
    }

    std::vector<std::uint8_t> MakeLine(std::uint8_t seed) {
        std::vector<std::uint8_t> bytes(kLineBytes);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(seed + i);
        }
        return bytes;
    }

    FrameView MakeFrame(const std::vector<std::uint8_t> &bytes, std::uint64_t block_id) {
        FrameView frame;
        frame.data = bytes.data();
        frame.size = bytes.size();
        frame.width = kSamples;
        frame.height = kBands;
        frame.bytes_per_pixel = 2;
        frame.block_id = block_id;
        return frame;
    }

    std::vector<std::uint8_t> ReadFile(const fs::path &path) {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(stream)),
                                         std::istreambuf_iterator<char>());
    }

    bool AnyPartFiles(const fs::path &dir) {
        for (const auto &entry: fs::recursive_directory_iterator(dir)) {
            if (entry.path().string().find(".part") != std::string::npos) return true;
        }
        return false;
    }
} // namespace

TEST_CASE("EnviRecorder: ByteExactBilHeaderAndLedger") {
    const fs::path tmp = MakeTempDir();
    Counters counters;
    EnviRecorder recorder(MakeConfig(tmp), counters);
    recorder.Start(MakeInit());

    const auto line1 = MakeLine(1);
    const auto line2 = MakeLine(50);
    const auto line3 = MakeLine(200);
    recorder.OnFrame(MakeFrame(line1, 10));
    recorder.OnFrame(MakeFrame(line2, 11));
    recorder.OnFrame(MakeFrame(line3, 12));
    recorder.Stop();

    const fs::path dir = recorder.SessionDir();
    REQUIRE(fs::exists(dir / "segment_0001.bil"));
    REQUIRE(fs::exists(dir / "segment_0001.hdr"));
    CHECK_FALSE(AnyPartFiles(dir));

    // .bil is the byte-for-byte concatenation of the frame payloads.
    std::vector<std::uint8_t> expected;
    expected.insert(expected.end(), line1.begin(), line1.end());
    expected.insert(expected.end(), line2.begin(), line2.end());
    expected.insert(expected.end(), line3.begin(), line3.end());
    CHECK(ReadFile(dir / "segment_0001.bil") == expected);
    CHECK(recorder.LinesWrittenTotal() == 3u);

    const auto hdr_bytes = ReadFile(dir / "segment_0001.hdr");
    const std::string hdr(hdr_bytes.begin(), hdr_bytes.end());
    CHECK(std::string(hdr).find("samples = 4") != std::string::npos);
    CHECK(std::string(hdr).find("lines = 3") != std::string::npos);
    CHECK(std::string(hdr).find("bands = 3") != std::string::npos);
    CHECK(std::string(hdr).find("interleave = bil") != std::string::npos);
    CHECK(std::string(hdr).find("wavelength source: list") != std::string::npos);

    CHECK(fx10::Classify(counters) == fx10::RunStatus::kClean);
    CHECK(counters.frames_written == 3u);
    CHECK(counters.segments_finalized == 1u);
    CHECK(counters.bytes_written == 3 * kLineBytes);
}

TEST_CASE("EnviRecorder: RotationByMaxLines") {
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.rotation.max_lines = 2;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeInit());

    const auto line = MakeLine(7);
    for (std::uint64_t id = 1; id <= 5; ++id) {
        recorder.OnFrame(MakeFrame(line, id));
    }
    recorder.Stop();

    const fs::path dir = recorder.SessionDir();
    for (const char *name: {"segment_0001", "segment_0002", "segment_0003"}) {
        CHECK_MESSAGE(fs::exists(dir / (std::string(name) + ".bil")), name);
        CHECK_MESSAGE(fs::exists(dir / (std::string(name) + ".hdr")), name);
    }
    CHECK(fs::file_size(dir / "segment_0001.bil") == 2 * kLineBytes);
    CHECK(fs::file_size(dir / "segment_0003.bil") == 1 * kLineBytes);

    CHECK(recorder.LinesWrittenTotal() == 5u); // continuous across segments
    CHECK(counters.segments_finalized == 3u);
}

TEST_CASE("EnviRecorder: GapRecordPolicyCountsWithoutPadding") {
    const fs::path tmp = MakeTempDir();
    Counters counters;
    EnviRecorder recorder(MakeConfig(tmp), counters);
    recorder.Start(MakeInit());

    const auto line = MakeLine(3);
    recorder.OnFrame(MakeFrame(line, 1));
    recorder.OnGap(2, 3); // blocks 2,3,4 lost
    recorder.OnFrame(MakeFrame(line, 5));
    recorder.Stop();

    const fs::path dir = recorder.SessionDir();
    CHECK(fs::file_size(dir / "segment_0001.bil") == 2 * kLineBytes);
    CHECK(counters.frames_missed_rx == 3u);
    CHECK(counters.blockid_gap_events == 1u);
    CHECK(counters.gap_lines_padded == 0u);
    CHECK(fx10::Classify(counters) == fx10::RunStatus::kDegraded);
}

TEST_CASE("EnviRecorder: GapPadZeroPolicyWritesZeroLines") {
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.on_gap = GapPolicy::kPadZero;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeInit());

    const auto line_a = MakeLine(1);
    const auto line_b = MakeLine(99);
    recorder.OnFrame(MakeFrame(line_a, 1));
    recorder.OnGap(2, 2);
    recorder.OnFrame(MakeFrame(line_b, 4));
    recorder.Stop();

    const fs::path dir = recorder.SessionDir();
    const auto bil = ReadFile(dir / "segment_0001.bil");
    REQUIRE(bil.size() == 4 * kLineBytes);
    const std::vector<std::uint8_t> zeros(kLineBytes, 0);
    CHECK(std::equal(bil.begin() + kLineBytes, bil.begin() + 2 * kLineBytes, zeros.begin()));
    CHECK(std::equal(bil.begin() + 2 * kLineBytes, bil.begin() + 3 * kLineBytes,
        zeros.begin()));

    // Padding keeps the BIL line index aligned with the trigger sequence.
    CHECK(recorder.LinesWrittenTotal() == 4u);
    CHECK(counters.gap_lines_padded == 2u);
    // bytes_written matches the .bil on disk, padded lines included.
    CHECK(counters.bytes_written == 4 * kLineBytes);
    CHECK(fx10::Classify(counters) == fx10::RunStatus::kDegraded); // frames were lost on RX
    // .hdr line count includes the padded lines (cube geometry).
    const auto hdr_bytes = ReadFile(dir / "segment_0001.hdr");
    CHECK(std::string(std::string(hdr_bytes.begin(), hdr_bytes.end())).find("lines = 4") != std::string::npos);
}

TEST_CASE("EnviRecorder: WriteFailureTruncatesFinalizesAndLatches") {
    const fs::path tmp = MakeTempDir();
    Counters counters;
    EnviRecorder recorder(MakeConfig(tmp), counters);

    recorder.Start(MakeInit());
    const auto line = MakeLine(1);
    recorder.OnFrame(MakeFrame(line, 1));
    int calls = 0;
    recorder.SetWriteHookForTest([&calls](int fd, const void *buf, std::size_t n) -> ssize_t {
        // Fail frame 2 data once; allow finalization of frame 1 and its index.
        if (++calls == 1) {
            errno = ENOSPC;
            return -1;
        }
        return ::write(fd, buf, n);
    });

    recorder.OnFrame(MakeFrame(line, 2)); // fails, latches
    CHECK(recorder.Failed());
    CHECK(std::string(recorder.ErrorMessage()).find("write failed") != std::string::npos);
    recorder.OnFrame(MakeFrame(line, 3)); // ignored after latch
    recorder.Stop();

    const fs::path dir = recorder.SessionDir();
    // Finalized partial segment: exactly 1 complete line, valid .hdr, no .part files.
    REQUIRE(fs::exists(dir / "segment_0001.bil"));
    CHECK(fs::file_size(dir / "segment_0001.bil") == 1 * kLineBytes);
    const auto hdr_bytes = ReadFile(dir / "segment_0001.hdr");
    CHECK(std::string(std::string(hdr_bytes.begin(), hdr_bytes.end())).find("lines = 1") != std::string::npos);
    CHECK_FALSE(AnyPartFiles(dir));

    CHECK(counters.write_errors >= 1u);
    CHECK(counters.frames_written == 1u);
    CHECK(recorder.Failed()); // the run counts as FAILED, not merely degraded
}

TEST_CASE("EnviRecorder: LineIdentitySurvivesPaddingAnomaliesAndRotation") {
    auto config = MakeConfig(MakeTempDir());
    config.on_gap = GapPolicy::kPadZero;
    config.rotation.max_lines = 2;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeInit());
    const auto bytes = MakeLine(0x31);
    auto frame = MakeFrame(bytes, 100);
    frame.device_timestamp_raw = 9007199254740993ULL; // cannot round-trip via a double
    frame.host_receive_realtime_ns = 1800000000000000001ULL;
    frame.host_receive_monotonic_ns = 123456789;
    recorder.OnFrame(frame);
    recorder.OnGap(101, 2);
    frame.block_id = 103;
    frame.block_id_anomaly = true;
    recorder.OnFrame(frame);
    recorder.Stop();
    CHECK_FALSE(recorder.Failed());
    const auto read_text = [](const fs::path &p) {
        const auto data = ReadFile(p);
        return std::string(data.begin(), data.end());
    };
    const auto first = read_text(recorder.SessionDir() / "segment_0001.lines.csv");
    const auto second = read_text(recorder.SessionDir() / "segment_0002.lines.csv");
    CHECK(first.find("frame,0,0,0,100,1,9007199254740993,1800000000000000001,123456789,0,") != std::string::npos);
    CHECK(first.find("gap,1,1,24,101,2,") != std::string::npos);
    CHECK(first.find("padding,1,1,24,0,1,") != std::string::npos);
    CHECK(second.find("padding,0,2,0,0,1,") != std::string::npos);
    CHECK(second.find("frame,1,3,24,103,1,9007199254740993,1800000000000000001,123456789,1,") != std::string::npos);
    CHECK(fs::file_size(recorder.SessionDir() / "segment_0001.bil") == 2 * kLineBytes);
    CHECK(fs::file_size(recorder.SessionDir() / "segment_0002.bil") == 2 * kLineBytes);
    CHECK(counters.frames_written == 2);
    CHECK(counters.gap_lines_padded == 2);
    std::ifstream summaries(recorder.SessionDir() / "segments.jsonl");
    std::string summary_line;
    REQUIRE(static_cast<bool>(std::getline(summaries, summary_line)));
    const auto s1 = nlohmann::json::parse(summary_line);
    REQUIRE(static_cast<bool>(std::getline(summaries, summary_line)));
    const auto s2 = nlohmann::json::parse(summary_line);
    CHECK_FALSE(static_cast<bool>(std::getline(summaries, summary_line))); // empty trailing segment omitted
    CHECK(s1["lines"] == 2);
    CHECK(s1["frames"] == 1);
    CHECK(s1["padding_lines"] == 1);
    CHECK(s1["gap_frames_reported"] == 2); // gap event belongs to the segment where observed
    CHECK(s1["global_line_end_exclusive"] == 2);
    CHECK(s1["first_frame"]["device_timestamp_raw"].get<std::uint64_t>() == 9007199254740993ULL);
    CHECK(s2["global_line_first"] == 2);
    CHECK(s2["last_frame"]["global_line"] == 3);
    CHECK(s2["padding_lines"] == 1);
    CHECK(s2["block_id_anomalies"] == 1);
}

TEST_CASE("EnviRecorder: SdkLayoutAndRejectedBufferDetailsSurviveInV2Index") {
    Counters counters;
    EnviRecorder recorder(MakeConfig(MakeTempDir()), counters);
    recorder.Start(MakeInit());
    const auto bytes = MakeLine(5);
    auto frame = MakeFrame(bytes, 7);
    frame.sdk = fx10::SdkFrameMetadata{};
    auto &sdk = *frame.sdk;
    sdk.acquired_size = 30;
    sdk.payload_type = 1;
    sdk.image_present = true;
    sdk.pixel_type = 42; // serialization test, not a PFNC claim
    sdk.width = 4;
    sdk.height = 3;
    sdk.padding_x = 2;
    sdk.image_size = 30;
    sdk.effective_image_size = 24;
    recorder.OnFrame(frame);
    frame.block_id = 8;
    sdk.operation_result = 17; // opaque SDK code
    sdk.image_present = false;
    recorder.OnRejected(frame.block_id, "operation-error", &frame);
    recorder.Stop();
    const auto bytes_index = ReadFile(recorder.SessionDir() / "segment_0001.lines.csv");
    const std::string text(bytes_index.begin(), bytes_index.end());
    CHECK(text.find("# fx10-line-index-v2;") == 0);
    CHECK(text.find(",30,1,0,0,1,42,4,3,2,0,30,24\n") != std::string::npos);
    CHECK(text.find(",30,1,17,0,0,,,,,,,\n") != std::string::npos);
    std::istringstream rows(text);
    std::string row;
    std::getline(rows, row); // preamble
    while (std::getline(rows, row)) CHECK(std::count(row.begin(), row.end(), ',') == 21);
    std::ifstream summary(recorder.SessionDir() / "segments.jsonl");
    std::getline(summary, row);
    const auto doc = nlohmann::json::parse(row);
    CHECK(doc["frames"] == 1);
    CHECK(doc["rejected_buffers"] == 1);
    CHECK(doc["last_frame"]["block_id"] == 7);
}

TEST_CASE("EnviRecorder: SummaryFailureDoesNotUncommitTheFinalizedPixels") {
    auto config = MakeConfig(MakeTempDir());
    config.rotation.max_lines = 1;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeInit());
    recorder.SetSummaryWriteHookForTest([](int, const void *, std::size_t) -> ssize_t {
        errno = ENOSPC;
        return -1;
    });
    const auto bytes = MakeLine(9);
    recorder.OnFrame(MakeFrame(bytes, 1));
    recorder.Stop();
    CHECK(recorder.Failed());
    CHECK(recorder.GetErrorKind() == fx10::ErrorKind::kIo);
    CHECK(counters.frames_written == 1);
    CHECK(counters.segments_finalized == 1);
    CHECK(ReadFile(recorder.SessionDir() / "segment_0001.bil") == bytes);
    CHECK(fs::exists(recorder.SessionDir() / "segment_0001.hdr"));
    CHECK(fs::file_size(recorder.SessionDir() / "segments.jsonl") == 0);
    CHECK_FALSE(fs::exists(recorder.SessionDir() / "segment_0002.bil.part"));
}

TEST_CASE("EnviRecorder: ShortIndexWriteRollsPixelsBackToTheLastCommittedPair") {
    Counters counters;
    EnviRecorder recorder(MakeConfig(MakeTempDir()), counters);
    recorder.Start(MakeInit());
    const auto bytes = MakeLine(0x44);
    recorder.OnFrame(MakeFrame(bytes, 7));
    int calls = 0;
    recorder.SetWriteHookForTest([&](int fd, const void *p, std::size_t n) -> ssize_t {
        ++calls;
        if (calls == 2) return ::write(fd, p, n / 2); // partial CSV record after complete pixel write
        if (calls == 3) { errno = EIO; return -1; }
        return ::write(fd, p, n);
    });
    recorder.OnFrame(MakeFrame(bytes, 8));
    recorder.Stop();
    CHECK(recorder.Failed());
    CHECK(counters.frames_written == 1);
    CHECK(fs::file_size(recorder.SessionDir() / "segment_0001.bil") == kLineBytes);
    const auto idx = ReadFile(recorder.SessionDir() / "segment_0001.lines.csv");
    const std::string text(idx.begin(), idx.end());
    CHECK(text.find("frame,0,0,0,7,1,") != std::string::npos);
    CHECK(text.find("frame,1,1,") == std::string::npos);
    CHECK_FALSE(AnyPartFiles(recorder.SessionDir()));
}

TEST_CASE("EnviRecorder: RejectedLeadingBuffersRemainInAnEventOnlySegment") {
    Counters counters;
    EnviRecorder recorder(MakeConfig(MakeTempDir()), counters);
    recorder.Start(MakeInit());
    recorder.OnRejected(55, "operation-error");
    recorder.OnGap(56, 10001); // record policy: full gap, no synthetic pixels
    recorder.Stop();
    CHECK_FALSE(recorder.Failed());
    const auto idx = ReadFile(recorder.SessionDir() / "segment_0001.lines.csv");
    const std::string text(idx.begin(), idx.end());
    CHECK(text.find("operation-error,0,0,0,55,1,") != std::string::npos);
    CHECK(text.find("gap,0,0,0,56,10001,") != std::string::npos);
    CHECK(fs::file_size(recorder.SessionDir() / "segment_0001.bil") == 0);
}

TEST_CASE("EnviRecorder: SizeMismatchDropped") {
    const fs::path tmp = MakeTempDir();
    Counters counters;
    EnviRecorder recorder(MakeConfig(tmp), counters);
    recorder.Start(MakeInit());

    std::vector<std::uint8_t> short_line(kLineBytes - 2, 1);
    FrameView bad = MakeFrame(short_line, 1);
    bad.size = short_line.size();
    recorder.OnFrame(bad);

    const auto line = MakeLine(2);
    recorder.OnFrame(MakeFrame(line, 2));
    recorder.Stop();

    CHECK(counters.size_mismatch_drops == 1u);
    CHECK(counters.frames_written == 1u);
    CHECK(recorder.LinesWrittenTotal() == 1u);
}

TEST_CASE("EnviRecorder: ConsecutiveSizeMismatchAborts") {
    const fs::path tmp = MakeTempDir();
    Counters counters;
    EnviRecorder recorder(MakeConfig(tmp), counters);
    recorder.Start(MakeInit());

    std::vector<std::uint8_t> short_line(kLineBytes - 2, 1);
    for (int i = 0; i < 26 && !recorder.Failed(); ++i) {
        FrameView bad = MakeFrame(short_line, static_cast<std::uint64_t>(i + 1));
        bad.size = short_line.size();
        recorder.OnFrame(bad);
    }
    CHECK(recorder.Failed());
    CHECK(std::string(recorder.ErrorMessage()).find("consecutive") != std::string::npos);
    CHECK(counters.size_mismatch_drops == 26u);
    recorder.Stop();
    CHECK(recorder.Failed());
}

TEST_CASE("EnviRecorder: EmptyTrailingSegmentRemoved") {
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.rotation.max_lines = 1;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeInit());
    recorder.OnFrame(MakeFrame(MakeLine(1), 1)); // finalizes seg 1, opens seg 2
    recorder.Stop(); // seg 2 has 0 lines -> removed

    const fs::path dir = recorder.SessionDir();
    CHECK(fs::exists(dir / "segment_0001.bil"));
    CHECK_FALSE(fs::exists(dir / "segment_0002.bil"));
    CHECK_FALSE(AnyPartFiles(dir));
    CHECK(counters.segments_finalized == 1u);
}

TEST_CASE("EnviRecorder: SessionDirNoClobber") {
    const fs::path tmp = MakeTempDir();
    const fs::path first = createSessionDir(tmp, "x", "20260721T000000Z");
    const fs::path second = createSessionDir(tmp, "x", "20260721T000000Z");
    CHECK(fs::exists(first));
    CHECK(fs::exists(second));
    CHECK(first != second);
    CHECK(second.filename().string() == first.filename().string() + "_1");
}

TEST_CASE("EnviRecorder: StartValidation") {
    const fs::path tmp = MakeTempDir();
    Counters counters;

    {
        EnviRecorder recorder(MakeConfig(tmp), counters);
        RecorderInit init = MakeInit();
        init.samples = 0;
        CHECK_THROWS_AS(recorder.Start(init), RecorderError);
    }
    {
        EnviRecorder recorder(MakeConfig(tmp), counters);
        RecorderInit init = MakeInit();
        init.bytes_per_pixel = 3;
        CHECK_THROWS_AS(recorder.Start(init), RecorderError);
    }
    {
        EnviRecorder recorder(MakeConfig(tmp), counters);
        RecorderInit init = MakeInit();
        init.bytes_per_pixel = 1; // mismatch: data_type still uint16
        CHECK_THROWS_AS(recorder.Start(init), RecorderError);
    }
    {
        EnviRecorder recorder(MakeConfig(tmp), counters);
        RecorderInit init = MakeInit();
        init.wavelengths.nm = {400.0}; // 1 value, 3 bands
        CHECK_THROWS_AS(recorder.Start(init), RecorderError);
    }
}

// --- rotation by size -------------------------------------------------------

namespace {
    // A realistic FX10e line (1024 samples x 224 bands x 2 B = 448 KiB), so a
    // megabyte-scale rotation/flush threshold is reached in a handful of frames
    // instead of tens of thousands.
    constexpr std::uint32_t kBigSamples = 1024;
    constexpr std::uint32_t kBigBands = 224;
    constexpr std::size_t kBigLineBytes = static_cast<std::size_t>(kBigSamples) * kBigBands * 2;

    RecorderInit MakeBigInit() {
        RecorderInit init;
        init.samples = kBigSamples;
        init.bands = kBigBands;
        init.bytes_per_pixel = 2;
        init.data_type = EnviDataType::kUint16;
        init.wavelengths.source_tag = "grid"; // no nm values: the .hdr omits the axis
        init.description = "unit test";
        return init;
    }

    FrameView MakeBigFrame(const std::vector<std::uint8_t> &bytes, std::uint64_t block_id) {
        FrameView frame;
        frame.data = bytes.data();
        frame.size = bytes.size();
        frame.width = kBigSamples;
        frame.height = kBigBands;
        frame.bytes_per_pixel = 2;
        frame.block_id = block_id;
        return frame;
    }

    std::size_t CountSegments(const fs::path &dir, const char *extension) {
        std::size_t n = 0;
        for (const auto &entry: fs::directory_iterator(dir)) {
            if (entry.path().extension() == extension) ++n;
        }
        return n;
    }
} // namespace

TEST_CASE("EnviRecorder: RotationByMaxMegabytes") {
    // The default rotation is by SIZE, because a line is 448 KiB at 1x1 binning
    // and 56 KiB at 8x — a line count means a wildly different segment size (and
    // a wildly different worst-case loss) depending on the optics settings.
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.rotation.max_mb = 1; // 1 MiB -> rotates every 3 lines (448 KiB each)
    config.flush_interval_mb = 0;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeBigInit());

    const std::vector<std::uint8_t> line(kBigLineBytes, 0xA5);
    for (std::uint64_t id = 1; id <= 7; ++id) {
        recorder.OnFrame(MakeBigFrame(line, id));
    }
    recorder.Stop();
    CHECK_FALSE(recorder.Failed());

    const fs::path dir = recorder.SessionDir();
    // 3 lines = 1.3 MiB >= 1 MiB, so: 3 + 3 + 1.
    CHECK(fs::file_size(dir / "segment_0001.bil") == 3 * kBigLineBytes);
    CHECK(fs::file_size(dir / "segment_0002.bil") == 3 * kBigLineBytes);
    CHECK(fs::file_size(dir / "segment_0003.bil") == 1 * kBigLineBytes);
    CHECK(counters.segments_finalized == 3u);
    CHECK(CountSegments(dir, ".hdr") == 3u); // every segment carries its validity marker
    CHECK_FALSE(AnyPartFiles(dir));
    CHECK(recorder.LinesWrittenTotal() == 7u);
}

// --- durability -------------------------------------------------------------

TEST_CASE("EnviRecorder: PeriodicFlushHappensOnTheConfiguredByteCadence") {
    // Without this, durability happened only at segment boundaries: with the
    // old 100000-line default that is a ~45 GB window, and a segment without
    // its .hdr is INVALID, so a power cut cost the whole thing.
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.flush_interval_mb = 1; // every 1 MiB -> every 3 lines (448 KiB each)
    Counters counters;
    EnviRecorder recorder(config, counters);

    // The periodic flush runs on the recorder's helper thread: atomic counter,
    // and a drain before every assertion.
    std::atomic<int> syncs{0};
    recorder.SetSyncHookForTest([&syncs](int fd) {
        ++syncs;
        return ::fdatasync(fd);
    });
    recorder.Start(MakeBigInit());
    syncs = 0; // capture.json is durably published at Start

    const std::vector<std::uint8_t> line(kBigLineBytes, 0x11);
    for (std::uint64_t id = 1; id <= 2; ++id) {
        recorder.OnFrame(MakeBigFrame(line, id));
    }
    recorder.DrainPendingFlushesForTest();
    CHECK_MESSAGE((syncs == 0), "under the cadence: no flush yet");
    recorder.OnFrame(MakeBigFrame(line, 3)); // 1.3 MiB written
    recorder.DrainPendingFlushesForTest();
    CHECK(syncs == 2); // pixels and identity index
    for (std::uint64_t id = 4; id <= 5; ++id) {
        recorder.OnFrame(MakeBigFrame(line, id));
    }
    recorder.DrainPendingFlushesForTest();
    CHECK_MESSAGE((syncs == 2), "the cadence counts from the LAST flush, not from the segment start");
    recorder.OnFrame(MakeBigFrame(line, 6)); // 2.6 MiB written
    recorder.DrainPendingFlushesForTest();
    CHECK(syncs == 4);

    recorder.Stop();
    CHECK_FALSE(recorder.Failed());
    CHECK(syncs > 4); // finalize syncs both files and the .hdr too
}

TEST_CASE("EnviRecorder: FlushIntervalZeroKeepsTheOldSegmentBoundaryBehaviour") {
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.flush_interval_mb = 0;
    Counters counters;
    EnviRecorder recorder(config, counters);

    std::atomic<int> syncs{0};
    recorder.SetSyncHookForTest([&syncs](int fd) {
        ++syncs;
        return ::fdatasync(fd);
    });
    recorder.Start(MakeBigInit());
    syncs = 0;
    const std::vector<std::uint8_t> line(kBigLineBytes, 0x22);
    for (std::uint64_t id = 1; id <= 8; ++id) {
        recorder.OnFrame(MakeBigFrame(line, id));
    }
    CHECK(syncs == 0);
    recorder.Stop();
    CHECK(syncs > 0); // ...but the segment finalize still syncs
}

TEST_CASE("EnviRecorder: FailedPeriodicFlushStopsTheRunEvenIfFinalizeCanRecover") {
    // Finalize retries to preserve usable data, but must not erase the earlier
    // failure of the durability contract or report this session as successful.
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.flush_interval_mb = 1;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeBigInit());

    std::atomic<int> syncs{0};
    recorder.SetSyncHookForTest([&syncs](int fd) -> int {
        // Fail only the periodic flushes; let the finalize succeed.
        if (++syncs <= 2) {
            errno = EIO;
            return -1;
        }
        return ::fdatasync(fd);
    });
    const std::vector<std::uint8_t> line(kBigLineBytes, 0x33);
    for (std::uint64_t id = 1; id <= 6; ++id) {
        recorder.OnFrame(MakeBigFrame(line, id));
    }
    recorder.DrainPendingFlushesForTest();
    CHECK(recorder.Failed());
    CHECK(recorder.GetErrorKind() == fx10::ErrorKind::kIo);
    // The flush runs on the helper thread; its failures reach the ledger when
    // the segment finalizes (single-writer rule), i.e. at Stop().
    recorder.Stop();
    CHECK(counters.write_errors >= 1u); // the two failed syncs of one flush request count once
    CHECK(recorder.Failed());
    CHECK(counters.segments_finalized == 1u);
    CHECK(fs::exists(recorder.SessionDir() / "segment_0001.hdr"));
}

// --- finalize failures ------------------------------------------------------

TEST_CASE("EnviRecorder: HeaderWriteFailureMarksTheRunFailed") {
    // The .hdr IS the validity marker: a segment without one cannot be read, so
    // failing to write it must reach the exit code instead of ending CLEAN.
    const fs::path tmp = MakeTempDir();
    Counters counters;
    EnviRecorder recorder(MakeConfig(tmp), counters);
    recorder.Start(MakeInit());

    const auto line = MakeLine(5);
    recorder.OnFrame(MakeFrame(line, 1));

    // From here on, only the .hdr is still to be written.
    recorder.SetWriteHookForTest([](int, const void *, std::size_t) -> ssize_t {
        errno = ENOSPC;
        return -1;
    });
    recorder.Stop();

    const fs::path dir = recorder.SessionDir();
    CHECK(fs::exists(dir / "segment_0001.bil"));
    CHECK_FALSE(fs::exists(dir / "segment_0001.hdr"));
    CHECK_FALSE(AnyPartFiles(dir)); // the .hdr.part is cleaned up
    CHECK(counters.segments_finalized == 0u);
    CHECK(recorder.Failed());
    CHECK(recorder.GetErrorKind() == fx10::ErrorKind::kIo);
    CHECK(std::string(recorder.ErrorMessage()).find("segment_0001.hdr") != std::string::npos);
}

TEST_CASE("EnviRecorder: FinalizeSyncFailureMarksTheRunFailed") {
    const fs::path tmp = MakeTempDir();
    Counters counters;
    EnviRecorder recorder(MakeConfig(tmp), counters);
    recorder.Start(MakeInit());
    recorder.SetSyncHookForTest([](int) -> int {
        errno = EIO;
        return -1;
    });
    recorder.OnFrame(MakeFrame(MakeLine(9), 1));
    recorder.Stop();

    CHECK(recorder.Failed());
    CHECK(recorder.GetErrorKind() == fx10::ErrorKind::kIo);
    CHECK(counters.segments_finalized == 0u);
    // The .part file is deliberately left behind: an unsynced segment must not
    // look like a finished one.
    CHECK(AnyPartFiles(recorder.SessionDir()));
}

// --- gap padding ------------------------------------------------------------

TEST_CASE("EnviRecorder: PadZeroIsCappedPerGap") {
    // A pathological gap (a link that dropped for minutes) must not turn into
    // an unbounded run of synthetic lines that fills the disk.
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.on_gap = GapPolicy::kPadZero;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeInit());

    recorder.OnFrame(MakeFrame(MakeLine(1), 1));
    recorder.OnGap(2, 25000); // far beyond the 10000-line cap
    recorder.Stop();

    CHECK(counters.gap_lines_padded == 10000u);
    CHECK(counters.frames_missed_rx == 25000u); // the loss is still reported in full
    CHECK(recorder.LinesWrittenTotal() == 1u + 10000u);
    CHECK_FALSE(recorder.Failed());
    CHECK(fs::file_size(recorder.SessionDir() / "segment_0001.bil") == 10001 * kLineBytes);
}

TEST_CASE("EnviRecorder: LedgerIdentityHoldsAcrossGapsAndDrops") {
    // Stop() cross-checks retrieve_ok == frames_written + size_mismatch_drops.
    // Padded gap lines must NOT be counted as written frames, or the check
    // fires on every healthy run with a gap in it.
    const fs::path tmp = MakeTempDir();
    RecordingConfig config = MakeConfig(tmp);
    config.on_gap = GapPolicy::kPadZero;
    Counters counters;
    EnviRecorder recorder(config, counters);
    recorder.Start(MakeInit());

    const auto line = MakeLine(2);
    std::vector<std::uint8_t> short_line(kLineBytes - 2, 0);
    for (std::uint64_t id = 1; id <= 3; ++id) {
        ++counters.retrieve_ok;
        recorder.OnFrame(MakeFrame(line, id));
    }
    recorder.OnGap(4, 2);
    ++counters.retrieve_ok;
    recorder.OnFrame(MakeFrame(short_line, 6)); // size mismatch -> dropped
    recorder.Stop();

    CHECK(counters.frames_written == 3u);
    CHECK(counters.size_mismatch_drops == 1u);
    CHECK(counters.gap_lines_padded == 2u);
    CHECK(counters.frames_written + counters.size_mismatch_drops == counters.retrieve_ok);
}
