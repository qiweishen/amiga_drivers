/// @file test_file_writer.cpp
/// @brief Tests for BufferedFileWriter and RotatingFileWriter (pure file
/// logic, exercised against a temp directory).

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "file_writer.h"


namespace {
    namespace fs = std::filesystem;

    fs::path MakeTempDir(const std::string &tag) {
        const auto dir = fs::temp_directory_path() / ("common_fw_" + tag);
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }

    std::vector<char> ReadAll(const fs::path &p) {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
} // namespace

TEST_CASE("RotatingFileWriter close failure stays latched across rotation and repeated close") {
    const auto dir = MakeTempDir("close_failure");
    common::RotatingFileWriter::Options opts;
    opts.make_path = [&dir](std::uint32_t seq) { return (dir / std::to_string(seq)).string(); };
    std::ofstream *stream = nullptr;
    opts.on_new_file = [&stream](std::ofstream &file) {
        stream = &file;
        return true;
    };
    common::RotatingFileWriter writer(std::move(opts));
    REQUIRE(writer.Append("tail", 4));
    REQUIRE(stream != nullptr);
    stream->setstate(std::ios::badbit); // simulate the stream's final commit failure
    CHECK_FALSE(writer.Close());
    CHECK(writer.Failed());
    CHECK_FALSE(writer.EndSegment());
    CHECK_FALSE(writer.OpenNext());
    CHECK_FALSE(writer.Append("later", 5));
    CHECK(writer.GetStats().files_opened == 1);
}


TEST_CASE(

    "BufferedFileWriter open failure is detectable"
) {
    common::BufferedFileWriter w;
    CHECK_FALSE(w.Open("/nonexistent-dir-xyz/file.bin", 4096));
    CHECK_FALSE(w.IsOpen());

    const auto dir = MakeTempDir("buffered");
    CHECK(w.Open((dir / "a.bin").string(), 4096));
    CHECK(w.IsOpen());
    w.Write("abc", 3);
    w.Close();
    CHECK_FALSE(w.IsOpen());
    CHECK(ReadAll(dir / "a.bin") == std::vector<char>{ 'a', 'b', 'c' });
}


TEST_CASE(

    "RotatingFileWriter rotates by size and writes per-file headers"
) {
    const auto dir = MakeTempDir("rotate");
    common::RotatingFileWriter::Options opts;
    opts.make_path = [&dir](std::uint32_t seq) { return (dir / ("seg_" + std::to_string(seq) + ".bin")).string(); };
    opts.max_file_bytes = 10; // header(2) + records
    opts.on_new_file = [](std::ofstream &f) {
        f.write("HH", 2);
        return static_cast<bool>(f);
    };
    common::RotatingFileWriter w(std::move(opts));

    // 4-byte records: header 2 + 4 + 4 = 10 >= 10 -> rotate after the 2nd
    CHECK(w.Append("AAAA", 4));
    CHECK(w.CurrentFileBytes() == 6); // header counted towards the threshold
    CHECK(w.Append("BBBB", 4)); // hits 10 -> rotates, next file opened
    CHECK(w.Append("CCCC", 4));
    w.Close();

    CHECK(ReadAll(dir / "seg_0.bin") == std::vector<char>{ 'H', 'H', 'A', 'A', 'A', 'A', 'B', 'B', 'B', 'B' });
    CHECK(ReadAll(dir / "seg_1.bin") == std::vector<char>{ 'H', 'H', 'C', 'C', 'C', 'C' });

    const auto &s = w.GetStats();
    CHECK(s.records_written == 3);
    CHECK(s.bytes_written == 12); // header bytes excluded
    CHECK(s.files_opened == 2);
}


TEST_CASE(

    "RotatingFileWriter precheck cap keeps files strictly under the limit"
) {
    const auto dir = MakeTempDir("precheck");
    common::RotatingFileWriter::Options opts;
    opts.make_path = [&dir](std::uint32_t seq) { return (dir / ("f" + std::to_string(seq) + ".bin")).string(); };
    opts.max_file_bytes = 10;
    opts.precheck_size_cap = true;
    common::RotatingFileWriter w(std::move(opts));

    CHECK(w.Append("AAAAAA", 6)); // 6 < 10, stays
    CHECK(w.Append("BBBBBB", 6)); // 6+6 > 10 -> pre-rotate: file0 stays at 6 bytes
    w.Close();
    CHECK(ReadAll(dir / "f0.bin").size() == 6);
    CHECK(ReadAll(dir / "f1.bin").size() == 6);
}


TEST_CASE(

    "RotatingFileWriter lazy open and EndSegment start fresh files"
) {
    const auto dir = MakeTempDir("segment");
    common::RotatingFileWriter::Options opts;
    opts.make_path = [&dir](std::uint32_t seq) { return (dir / ("s" + std::to_string(seq) + ".bin")).string(); };
    common::RotatingFileWriter w(std::move(opts));

    CHECK_FALSE(w.IsOpen()); // lazy: nothing opened yet
    CHECK(w.Append("xy", 2));
    CHECK(w.IsOpen());
    w.EndSegment();
    CHECK_FALSE(w.IsOpen());
    CHECK(w.Append("z", 1)); // new segment -> next sequence file
    w.Close();
    CHECK(fs::exists(dir / "s0.bin"));
    CHECK(fs::exists(dir / "s1.bin"));
    CHECK(w.GetStats().files_opened == 2);
}


TEST_CASE(

    "RotatingFileWriter reports open failure"
) {
    common::RotatingFileWriter::Options opts;
    opts.make_path = [](std::uint32_t) { return std::string("/nonexistent-dir-xyz/f.bin"); };
    common::RotatingFileWriter w(std::move(opts));
    CHECK_FALSE(w.OpenNext());
    CHECK_FALSE(w.Append("a", 1));
}

TEST_CASE(

    "RotatingFileWriter hard cap refuses a record that can never fit"
) {
    const auto dir = MakeTempDir("hardcap");
    common::RotatingFileWriter::Options opts;
    opts.make_path = [&dir](std::uint32_t seq) { return (dir / ("cap_" + std::to_string(seq) + ".bin")).string(); };
    opts.max_file_bytes = 8;
    opts.precheck_size_cap = true;
    common::RotatingFileWriter w(opts);
    CHECK(w.Append("12345", 5));
    CHECK(w.Append("678", 3)); // exactly the cap
    CHECK(w.Append("9", 1)); // rotates first
    CHECK_FALSE(w.Append("0123456789", 10)); // larger than any file may be: refused, not written
    w.Close();
    CHECK(w.GetStats().files_opened == 2u);
    CHECK(ReadAll(dir / "cap_0.bin").size() == 8u);
    CHECK(ReadAll(dir / "cap_1.bin").size() == 1u);
}


TEST_CASE(

    "RotatingFileWriter rotates by time on the next append"
) {
    const auto dir = MakeTempDir("time");
    common::RotatingFileWriter::Options opts;
    opts.make_path = [&dir](std::uint32_t seq) { return (dir / ("t_" + std::to_string(seq) + ".bin")).string(); };
    opts.rotate_interval = std::chrono::seconds(1);
    common::RotatingFileWriter w(opts);
    CHECK(w.Append("a", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(w.Append("b", 1));
    w.Close();
    CHECK(w.GetStats().files_opened == 2u);
    CHECK(fs::exists(dir / "t_1.bin"));
}
