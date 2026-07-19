// End-to-end test for the Recorder (core/recorder.cpp): writes frames of
// varying sizes into a temp directory with a tiny segment cap, then re-parses
// every produced file with plain fread/getline — the read side deliberately
// shares no code with the Recorder, so the on-disk format itself is what is
// being verified (as scripts/inspect_raw.py will see it).

#include "core/recorder.hpp"

#include "core/format.hpp"
#include "core/frame.hpp"
#include "core/stats.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kPfncMono8 = 0x01080001u;

std::string make_temp_dir() {
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base && *base ? base : "/tmp") + "/jai_recorder_test_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = ::mkdtemp(buf.data());
    REQUIRE(dir != nullptr);
    return std::string(dir);
}

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

} // namespace

TEST_CASE("recorder: segment layout, rotation, index and summaries survive a byte-level re-parse") {
    namespace fmt = jai::format;

    // Payload sizes chosen so that with segment_max_bytes = 8192 and
    // record_align = 512 the layout is fully known in advance. Derivation:
    // record = align_up(96 + size, 512), the file header occupies [0, 512),
    // and rotation triggers when a non-first record would end past 8192:
    //   frame 0 size  100 rec  512 -> seg 1 @  512
    //   frame 1 size  700 rec 1024 -> seg 1 @ 1024
    //   frame 2 size 3000 rec 3584 -> seg 1 @ 2048  (segment now 5632 bytes)
    //   frame 3 size 5000 rec 5120 -> seg 2 @  512  (5632 + 5120 > 8192: rotated)
    //   frame 4 size 9000 rec 9216 -> seg 3 @  512  (record alone exceeds the cap:
    //                                                still lands whole in its own segment)
    //   frame 5 size  256 rec  512 -> seg 4 @  512  (seg 3 already past the cap)
    //   frame 6 size  512 rec 1024 -> seg 4 @ 1024
    //   frame 7 size    1 rec  512 -> seg 4 @ 2048  (segment ends at 2560)
    const std::vector<size_t> sizes = {100, 700, 3000, 5000, 9000, 256, 512, 1};
    const uint32_t expect_seg[8] = {1, 1, 1, 2, 3, 4, 4, 4};
    const uint64_t expect_off[8] = {512, 1024, 2048, 512, 512, 512, 1024, 2048};
    const uint64_t expect_seg_frames[4] = {3, 1, 1, 3};
    const uint64_t expect_seg_bytes[4] = {5632, 5632, 9728, 2560};
    const uint64_t expect_total_record_bytes = 21504; // sum of the aligned record sizes

    const std::string tmp = make_temp_dir();
    uint8_t uuid[16];
    for (int i = 0; i < 16; ++i) {
        uuid[i] = static_cast<uint8_t>(0xA0 + i);
    }

    jai::RecorderOptions opts;
    opts.camera_dir = tmp + "/cam0";
    opts.camera_id = "cam0";
    opts.camera_serial = "FAKE-1234";
    std::memcpy(opts.session_uuid, uuid, 16);
    opts.segment_max_bytes = 8192;
    opts.record_align = 512;
    opts.payload_crc = true;
    // min_free_bytes = 0 disables the free-space check; ENOSPC cannot be
    // forced portably, so only the disabled path is exercised here.
    opts.min_free_bytes = 0;

    // ---- write phase --------------------------------------------------
    std::vector<std::vector<uint8_t>> payloads;
    jai::CameraStats stats;
    {
        jai::Recorder rec(opts, &stats);
        rec.open();
        rec.open(); // idempotent

        for (size_t i = 0; i < sizes.size(); ++i) {
            std::vector<uint8_t> payload(sizes[i]);
            for (size_t j = 0; j < payload.size(); ++j) {
                payload[j] = static_cast<uint8_t>(i * 31 + j);
            }
            jai::FrameMeta meta;
            meta.block_id = 1000 + i;
            meta.device_ts_ns = 1'000'000'000ull + i * 10'000'000ull;
            meta.host_realtime_ns = 1'752'000'000'000'000'000ull + i;
            meta.host_monotonic_ns = 5'000'000'000ull + i;
            meta.pixel_format = kPfncMono8;
            meta.width = 640;
            meta.height = 480;
            meta.status_flags = (i == 2) ? fmt::kFrameFlagIncomplete : 0;
            meta.payload_size = payload.size();
            meta.expected_size = payload.size();
            rec.write_frame(meta, payload.data(), payload.size());
            CHECK(rec.current_segment_index() == expect_seg[i]); // rotation at the expected frame
            payloads.push_back(std::move(payload));
        }
        CHECK(rec.frames_written() == 8u);
        rec.close();
        rec.close(); // idempotent

        jai::FrameMeta meta;
        CHECK_THROWS_AS(rec.write_frame(meta, nullptr, 0), jai::IoError); // closed
    }
    CHECK(stats.frames_written.load() == 8u);
    CHECK(stats.segments_created.load() == 4u);
    CHECK(stats.bytes_written.load() == expect_total_record_bytes);

    // ---- read phase: raw segments + per-segment index ------------------
    for (uint32_t seg = 1; seg <= 4; ++seg) {
        char name[32];
        std::snprintf(name, sizeof(name), "seg_%05u.raw", seg);
        const std::string seg_path = opts.camera_dir + "/" + name;
        CAPTURE(seg_path);

        const uint64_t file_size = std::filesystem::file_size(seg_path);
        CHECK(file_size == expect_seg_bytes[seg - 1]);

        std::FILE* f = std::fopen(seg_path.c_str(), "rb");
        REQUIRE(f != nullptr);

        fmt::FileHeader fh{};
        REQUIRE(std::fread(&fh, sizeof(fh), 1, f) == 1u);
        CHECK(std::memcmp(fh.file_magic, "JAIRAWSG", 8) == 0);
        CHECK(fh.file_header_size == fmt::kFileHeaderSize);
        CHECK(fh.version_major == 1);
        CHECK(fh.version_minor == 0);
        CHECK(fh.byte_order_mark == 0x0A0B0C0Du);
        CHECK(fh.segment_index == seg); // matches the file name
        CHECK(fh.created_realtime_ns > 0u);
        CHECK(std::memcmp(fh.session_uuid, uuid, 16) == 0);
        CHECK(std::string(fh.camera_id) == "cam0");
        CHECK(std::string(fh.camera_serial) == "FAKE-1234");
        CHECK(fh.frame_header_size == fmt::kFrameHeaderSize);
        CHECK(fh.record_align == 512u);
        CHECK((fh.segment_flags & fmt::kSegFlagPayloadCrc) != 0u);
        CHECK((fh.segment_flags & fmt::kSegFlagChunkData) == 0u); // always 0 in v1
        CHECK(fmt::verify_file_header(fh));

        // Walk the records exactly as an external reader would: the next
        // record starts at align_up(offset + 96 + payload_size, record_align).
        uint64_t off = fmt::kFileHeaderSize;
        uint64_t frames_in_seg = 0;
        for (size_t g = 0; g < sizes.size(); ++g) {
            if (expect_seg[g] != seg) {
                continue;
            }
            CAPTURE(g);
            CHECK(off == expect_off[g]);
            CHECK(off % 512u == 0u); // every record starts on a record_align boundary

            REQUIRE(std::fseek(f, static_cast<long>(off), SEEK_SET) == 0);
            fmt::FrameHeader h{};
            REQUIRE(std::fread(&h, sizeof(h), 1, f) == 1u);
            CHECK(h.frame_magic == fmt::kFrameMagic);
            CHECK(h.header_size == fmt::kFrameHeaderSize);
            CHECK(fmt::verify_frame_header(h));
            CHECK(h.block_id == 1000 + g);
            CHECK(h.device_ts_ns == 1'000'000'000ull + g * 10'000'000ull);
            CHECK(h.host_realtime_ns == 1'752'000'000'000'000'000ull + g);
            CHECK(h.host_monotonic_ns == 5'000'000'000ull + g);
            CHECK(h.pixel_format == kPfncMono8);
            CHECK(h.width == 640u);
            CHECK(h.height == 480u);
            CHECK(h.status_flags == ((g == 2) ? fmt::kFrameFlagIncomplete : 0u));
            CHECK(h.payload_size == sizes[g]);
            CHECK(h.frame_seq == g); // monotonic across segment rotation

            std::vector<uint8_t> payload(sizes[g]);
            REQUIRE(std::fread(payload.data(), 1, payload.size(), f) == payload.size());
            CHECK(payload == payloads[g]); // payload round-trips byte for byte
            CHECK(h.payload_crc32c == fmt::crc32c(payload.data(), payload.size()));

            // Padding up to the next boundary must be zero.
            const uint64_t rec_bytes = fmt::align_up(fmt::kFrameHeaderSize + sizes[g], 512);
            const size_t pad = static_cast<size_t>(rec_bytes - fmt::kFrameHeaderSize - sizes[g]);
            if (pad > 0) {
                std::vector<uint8_t> padding(pad);
                REQUIRE(std::fread(padding.data(), 1, pad, f) == pad);
                CHECK(static_cast<size_t>(std::count(padding.begin(), padding.end(), 0)) == pad);
            }

            off += rec_bytes;
            CHECK(off <= file_size); // the whole record fits: frames are never split
            ++frames_in_seg;
        }
        CHECK(frames_in_seg == expect_seg_frames[seg - 1]);
        CHECK(off == file_size); // no trailing garbage after the last record
        std::fclose(f);

        // Per-segment index: one JSON line per frame, matching the data.
        char idx_name[40];
        std::snprintf(idx_name, sizeof(idx_name), "seg_%05u.idx.jsonl", seg);
        const std::vector<std::string> lines = read_lines(opts.camera_dir + "/" + idx_name);
        REQUIRE(lines.size() == expect_seg_frames[seg - 1]);
        size_t li = 0;
        for (size_t g = 0; g < sizes.size(); ++g) {
            if (expect_seg[g] != seg) {
                continue;
            }
            CAPTURE(g);
            const nlohmann::json j = nlohmann::json::parse(lines[li]);
            CHECK(j.at("seq").get<uint64_t>() == g);
            CHECK(j.at("bid").get<uint64_t>() == 1000 + g);
            CHECK(j.at("dts").get<uint64_t>() == 1'000'000'000ull + g * 10'000'000ull);
            CHECK(j.at("hrt").get<uint64_t>() == 1'752'000'000'000'000'000ull + g);
            CHECK(j.at("hmn").get<uint64_t>() == 5'000'000'000ull + g);
            CHECK(j.at("off").get<uint64_t>() == expect_off[g]);
            CHECK(j.at("psz").get<uint64_t>() == sizes[g]);
            CHECK(j.at("pf").get<uint32_t>() == kPfncMono8);
            CHECK(j.at("w").get<uint32_t>() == 640u);
            CHECK(j.at("h").get<uint32_t>() == 480u);
            CHECK(j.at("fl").get<uint32_t>() == ((g == 2) ? 1u : 0u));
            ++li;
        }
    }

    // ---- read phase: segments.jsonl ------------------------------------
    const std::vector<std::string> seg_lines = read_lines(opts.camera_dir + "/segments.jsonl");
    REQUIRE(seg_lines.size() == 4u);
    struct SegExpect {
        uint64_t seq_first, seq_last, bid_first, bid_last;
    };
    const SegExpect seg_expect[4] = {
        {0, 2, 1000, 1002}, {3, 3, 1003, 1003}, {4, 4, 1004, 1004}, {5, 7, 1005, 1007}};
    for (size_t s = 0; s < 4; ++s) {
        CAPTURE(s);
        const nlohmann::json j = nlohmann::json::parse(seg_lines[s]);
        char name[32];
        std::snprintf(name, sizeof(name), "seg_%05u.raw", static_cast<unsigned>(s + 1));
        CHECK(j.at("seg").get<std::string>() == name);
        CHECK(j.at("frames").get<uint64_t>() == expect_seg_frames[s]);
        CHECK(j.at("bytes").get<uint64_t>() == expect_seg_bytes[s]);
        CHECK(j.at("seq_first").get<uint64_t>() == seg_expect[s].seq_first);
        CHECK(j.at("seq_last").get<uint64_t>() == seg_expect[s].seq_last);
        CHECK(j.at("bid_first").get<uint64_t>() == seg_expect[s].bid_first);
        CHECK(j.at("bid_last").get<uint64_t>() == seg_expect[s].bid_last);
        CHECK(j.at("dts_first").get<uint64_t>() ==
              1'000'000'000ull + seg_expect[s].seq_first * 10'000'000ull);
        CHECK(j.at("dts_last").get<uint64_t>() ==
              1'000'000'000ull + seg_expect[s].seq_last * 10'000'000ull);
        CHECK(j.at("closed_clean").get<bool>());
    }

    std::filesystem::remove_all(tmp);
}

TEST_CASE("recorder: payload_crc off leaves the crc field zero but headers stay sealed") {
    namespace fmt = jai::format;

    const std::string tmp = make_temp_dir();
    jai::RecorderOptions opts;
    opts.camera_dir = tmp + "/cam0";
    opts.camera_id = "cam0";
    opts.camera_serial = "";
    opts.segment_max_bytes = 1u << 20;
    opts.record_align = 1; // alignment disabled: records are packed back to back
    opts.payload_crc = false;

    std::vector<uint8_t> payload(1000);
    for (size_t j = 0; j < payload.size(); ++j) {
        payload[j] = static_cast<uint8_t>(j ^ 0x5A);
    }
    {
        jai::Recorder rec(opts, nullptr); // stats are optional
        rec.open();
        jai::FrameMeta meta;
        meta.block_id = 1;
        meta.payload_size = payload.size();
        rec.write_frame(meta, payload.data(), payload.size());
        rec.write_frame(meta, payload.data(), payload.size());
        rec.close();
    }

    const std::string seg_path = opts.camera_dir + "/seg_00001.raw";
    // With record_align = 1 there is no padding at all.
    CHECK(std::filesystem::file_size(seg_path) == 512u + 2u * (96u + 1000u));

    std::FILE* f = std::fopen(seg_path.c_str(), "rb");
    REQUIRE(f != nullptr);
    fmt::FileHeader fh{};
    REQUIRE(std::fread(&fh, sizeof(fh), 1, f) == 1u);
    CHECK((fh.segment_flags & fmt::kSegFlagPayloadCrc) == 0u);
    CHECK(fh.record_align == 1u);
    CHECK(fmt::verify_file_header(fh));

    for (int i = 0; i < 2; ++i) {
        fmt::FrameHeader h{};
        REQUIRE(std::fread(&h, sizeof(h), 1, f) == 1u);
        CHECK(h.frame_magic == fmt::kFrameMagic);
        CHECK(fmt::verify_frame_header(h)); // header CRC is always on
        CHECK(h.payload_crc32c == 0u);      // payload CRC disabled
        CHECK(h.frame_seq == static_cast<uint64_t>(i));
        REQUIRE(std::fseek(f, static_cast<long>(h.payload_size), SEEK_CUR) == 0);
    }
    std::fclose(f);

    std::filesystem::remove_all(tmp);
}
