/// @file test_file_writer.cpp
/// @brief Tests for BufferedFileWriter and RotatingFileWriter (pure file
/// logic, exercised against a temp directory).

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
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
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}
}  // namespace


TEST_CASE("BufferedFileWriter open failure is detectable") {
	Common::BufferedFileWriter w;
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


TEST_CASE("RotatingFileWriter rotates by size and writes per-file headers") {
	const auto dir = MakeTempDir("rotate");
	Common::RotatingFileWriter::Options opts;
	opts.make_path = [&dir](std::uint32_t seq) { return (dir / ("seg_" + std::to_string(seq) + ".bin")).string(); };
	opts.max_file_bytes = 10;  // header(2) + records
	opts.on_new_file = [](std::ofstream &f) {
		f.write("HH", 2);
		return static_cast<bool>(f);
	};
	Common::RotatingFileWriter w(std::move(opts));

	// 4-byte records: header 2 + 4 + 4 = 10 >= 10 -> rotate after the 2nd
	CHECK(w.Append("AAAA", 4));
	CHECK(w.CurrentFileBytes() == 6);  // header counted towards the threshold
	CHECK(w.Append("BBBB", 4));		   // hits 10 -> rotates, next file opened
	CHECK(w.Append("CCCC", 4));
	w.Close();

	CHECK(ReadAll(dir / "seg_0.bin") == std::vector<char>{ 'H', 'H', 'A', 'A', 'A', 'A', 'B', 'B', 'B', 'B' });
	CHECK(ReadAll(dir / "seg_1.bin") == std::vector<char>{ 'H', 'H', 'C', 'C', 'C', 'C' });

	const auto &s = w.GetStats();
	CHECK(s.records_written == 3);
	CHECK(s.bytes_written == 12);  // header bytes excluded
	CHECK(s.files_opened == 2);
}


TEST_CASE("RotatingFileWriter precheck cap keeps files strictly under the limit") {
	const auto dir = MakeTempDir("precheck");
	Common::RotatingFileWriter::Options opts;
	opts.make_path = [&dir](std::uint32_t seq) { return (dir / ("f" + std::to_string(seq) + ".bin")).string(); };
	opts.max_file_bytes = 10;
	opts.precheck_size_cap = true;
	Common::RotatingFileWriter w(std::move(opts));

	CHECK(w.Append("AAAAAA", 6));  // 6 < 10, stays
	CHECK(w.Append("BBBBBB", 6));  // 6+6 > 10 -> pre-rotate: file0 stays at 6 bytes
	w.Close();
	CHECK(ReadAll(dir / "f0.bin").size() == 6);
	CHECK(ReadAll(dir / "f1.bin").size() == 6);
}


TEST_CASE("RotatingFileWriter lazy open and EndSegment start fresh files") {
	const auto dir = MakeTempDir("segment");
	Common::RotatingFileWriter::Options opts;
	opts.make_path = [&dir](std::uint32_t seq) { return (dir / ("s" + std::to_string(seq) + ".bin")).string(); };
	Common::RotatingFileWriter w(std::move(opts));

	CHECK_FALSE(w.IsOpen());  // lazy: nothing opened yet
	CHECK(w.Append("xy", 2));
	CHECK(w.IsOpen());
	w.EndSegment();
	CHECK_FALSE(w.IsOpen());
	CHECK(w.Append("z", 1));  // new segment -> next sequence file
	w.Close();
	CHECK(fs::exists(dir / "s0.bin"));
	CHECK(fs::exists(dir / "s1.bin"));
	CHECK(w.GetStats().files_opened == 2);
}


TEST_CASE("RotatingFileWriter reports open failure") {
	Common::RotatingFileWriter::Options opts;
	opts.make_path = [](std::uint32_t) { return std::string("/nonexistent-dir-xyz/f.bin"); };
	Common::RotatingFileWriter w(std::move(opts));
	CHECK_FALSE(w.OpenNext());
	CHECK_FALSE(w.Append("a", 1));
}
