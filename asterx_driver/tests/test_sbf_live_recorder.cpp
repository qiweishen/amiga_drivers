// SPDX-License-Identifier: BSD-3-Clause
// The live CSV feeds the web GUI. It must never throw, and a Do-Not-Use value
// must reach the GUI as "no reading", not as a plausible number.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "sbf_frame_builder.h"
#include "sbf_live_recorder.h"

using asterx_test::FrameBuilder;
using asterx_test::ins_fixed_body;
using asterx_test::rxs_fixed_body;

namespace {
    std::filesystem::path Scratch(const std::string &name) {
        const auto p = std::filesystem::temp_directory_path() /
                       ("asterx_live_" + std::to_string(::getpid()) + "_" + name);
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }

    std::vector<std::string> ReadLines(const std::filesystem::path &file) {
        std::vector<std::string> lines;
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
        }
        return lines;
    }

    // field n of a comma-separated row
    std::string Field(const std::string &row, std::size_t index) {
        std::stringstream ss(row);
        std::string part;
        for (std::size_t i = 0; std::getline(ss, part, ','); ++i) {
            if (i == index) {
                return part;
            }
        }
        return {};
    }

    void Feed(asterx::SbfLiveRecorder &rec, const std::vector<std::uint8_t> &frame) {
        rec.OnBlock(frame.data(), frame.size());
    }
} // namespace

TEST_CASE("SbfLiveRecorder: ReceiverStatusRowCarriesPhysicalUnits") {
    const auto dir = Scratch("rxs");
    asterx::SbfLiveRecorder rec(asterx::SbfLiveRecorder::Config{dir.string(), true});

    FrameBuilder ok;
    rxs_fixed_body(ok, /*n=*/2, /*sb_length=*/4, /*up_time=*/1201,
                   /*rx_status=*/(1u << 6), /*temperature=*/142, /*cpu_load=*/35);
    ok.u8(1);
    ok.i8(50);
    ok.u8(0);
    ok.u8(0);
    ok.u8(2);
    ok.i8(-3);
    ok.u8(0);
    ok.u8(0);
    Feed(rec, ok.finalize(4014, 0));

    // Temperature carries an offset of 100 and a Do-Not-Use value of 0 (p.375)
    FrameBuilder dnu;
    rxs_fixed_body(dnu, 0, 0, 1202, 0, /*temperature=*/0, /*cpu_load=*/255);
    Feed(rec, dnu.finalize(4014, 0));
    rec.close();

    const auto lines = ReadLines(dir / "live_receiverstatus.csv");
    REQUIRE_MESSAGE((lines.size() == 3u), "header + two rows");
    CHECK(Field(lines[0], 4) == "cpu_load_pct");
    CHECK(Field(lines[0], 9) == "temp_c");
    CHECK(Field(lines[0], 11) == "agc");

    CHECK(Field(lines[1], 4) == "35");
    CHECK(Field(lines[1], 5) == "1201"); // up_time_s
    CHECK(Field(lines[1], 6) == "64"); // rx_status, FINETIME
    CHECK(Field(lines[1], 9) == "42"); // 142 - 100
    CHECK(Field(lines[1], 11) == "1:50;2:-3");

    CHECK_MESSAGE((Field(lines[2], 9) == "nan"), "a DNU temperature must not read as -100 degC");
    CHECK(Field(lines[2], 11) == ""); // no AGC sub-blocks
    CHECK(rec.GetStats().rows_written == 2u);
    CHECK(rec.GetStats().parse_errors == 0u);

    std::filesystem::remove_all(dir);
}

TEST_CASE("SbfLiveRecorder: InsRowKeepsWireResolutionAndDnu") {
    const auto dir = Scratch("ins");
    asterx::SbfLiveRecorder rec(asterx::SbfLiveRecorder::Config{dir.string(), true});

    FrameBuilder fb;
    // GNSSAge and Accuracy are u2 at 0.01 resolution, up to 655.34 (p.336)
    ins_fixed_body(fb, /*lat=*/0.55, /*gnss_age=*/12345, /*accuracy=*/8);
    fb.U16(0x02); // attitude group only
    fb.F32(90.5f);
    fb.F32(1.25f);
    fb.F32(-0.5f);
    Feed(rec, fb.finalize(4226, 0));

    FrameBuilder dnu;
    ins_fixed_body(dnu, 0.55, /*gnss_age=*/65535, /*accuracy=*/65535);
    dnu.U16(0);
    Feed(rec, dnu.finalize(4230, 0)); // ExtEventINSNavGeod shares the layout
    rec.close();

    const auto lines = ReadLines(dir / "live_insnavgeod.csv");
    REQUIRE(lines.size() == 3u);
    CHECK(Field(lines[0], 7) == "gnss_age_s");
    CHECK(Field(lines[0], 12) == "accuracy_m");

    CHECK_MESSAGE((Field(lines[1], 7) == "123.45"), "%.4g would have truncated this to 123.4");
    CHECK(Field(lines[1], 12) == "0.08");
    CHECK(Field(lines[1], 18) == "90.5"); // heading_deg
    CHECK(Field(lines[2], 7) == "nan");
    CHECK(Field(lines[2], 12) == "nan");
    CHECK(Field(lines[2], 18) == "nan"); // absent attitude group

    std::filesystem::remove_all(dir);
}

TEST_CASE("SbfLiveRecorder: IgnoresOtherBlocksAndCountsParseErrors") {
    const auto dir = Scratch("misc");
    asterx::SbfLiveRecorder rec(asterx::SbfLiveRecorder::Config{dir.string(), true});

    // A block the GUI does not need: .sbf-only, no CSV, no error
    FrameBuilder other;
    other.U32(0);
    Feed(rec, other.finalize(4007, 0));
    CHECK(rec.GetStats().rows_written == 0u);
    CHECK(rec.GetStats().parse_errors == 0u);

    // A truncated ReceiverStatus is counted, not fatal
    FrameBuilder bad;
    rxs_fixed_body(bad, /*n=*/4, /*sb_length=*/8); // promises sub-blocks the frame lacks
    Feed(rec, bad.finalize(4014, 0));
    CHECK(rec.GetStats().parse_errors == 1u);
    CHECK(rec.GetStats().rows_written == 0u);

    // Too short to even hold a block id
    const std::vector<std::uint8_t> stub{0x24, 0x40, 0, 0};
    rec.OnBlock(stub.data(), stub.size());
    rec.close();
    CHECK_FALSE(std::filesystem::exists(dir / "live_receiverstatus.csv"));

    std::filesystem::remove_all(dir);
}

TEST_CASE("SbfLiveRecorder: DisabledAndUnwritableAreNotFatal") {
    const auto dir = Scratch("off");
    asterx::SbfLiveRecorder off(asterx::SbfLiveRecorder::Config{dir.string(), false});
    FrameBuilder fb;
    rxs_fixed_body(fb, 0, 0);
    const auto frame = fb.finalize(4014, 0);
    off.OnBlock(frame.data(), frame.size());
    off.close();
    CHECK(off.GetStats().rows_written == 0u);
    CHECK_FALSE(std::filesystem::exists(dir / "live_receiverstatus.csv"));

    // A directory that does not exist disables the channel instead of throwing
    asterx::SbfLiveRecorder gone(asterx::SbfLiveRecorder::Config{(dir / "missing").string(), true});
    CHECK_NOTHROW(gone.OnBlock(frame.data(), frame.size()));
    CHECK_NOTHROW(gone.OnBlock(frame.data(), frame.size()));
    CHECK_NOTHROW(gone.Flush());
    CHECK_NOTHROW(gone.close());
    CHECK(gone.GetStats().rows_written == 0u);

    std::filesystem::remove_all(dir);
}
