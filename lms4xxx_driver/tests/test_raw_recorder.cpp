#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include "raw_recorder.h"

namespace {
    struct TempDirectory {
        std::filesystem::path path;

        TempDirectory() {
            static std::atomic<unsigned> next{0};
            path = std::filesystem::temp_directory_path() /
                ("lms4xxx_raw_" + std::to_string(::getpid()) + "_" + std::to_string(next.fetch_add(1)));
            REQUIRE(std::filesystem::create_directory(path));
        }

        ~TempDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }

        lms4xxx::RawRecorder::Config Config() const {
            lms4xxx::RawRecorder::Config config;
            config.output_path = (path / "scan_unit_20261030_120000.bin").string();
            config.instance = "unit";
            config.session_timestamp = "20261030_120000";
            config.metadata = {{"device_type", "fixture"}, {"scan", {{"remission", "rssi"}}}};
            config.queue_capacity = 16;
            return config;
        }

        std::filesystem::path Split(const char *suffix) const {
            return path / (std::string("scan_unit_20261030_120000_") + suffix);
        }
    };

    lms4xxx::RawRecord Record(const std::string &payload, bool scan, bool checksum_valid = true) {
        lms4xxx::RawRecord record;
        auto &data = record.frame.data;
        const auto size = static_cast<std::uint32_t>(payload.size());
        data = {2, 2, 2, 2, static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
                static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
        std::uint8_t checksum = 0;
        for (const unsigned char byte : payload) {
            data.push_back(byte);
            checksum ^= byte;
        }
        data.push_back(checksum_valid ? checksum : static_cast<std::uint8_t>(checksum ^ 1));
        record.frame.receive_timestamp_us = 1000001;
        record.frame.checksum_valid = checksum_valid;
        record.is_scan = scan;
        if (scan) {
            record.device_time_unix_us = 1700000000123456;
            record.clock_step_us = -13;
            record.clock_quality_flags = 1025;
        }
        return record;
    }

    std::vector<std::uint8_t> Bytes(const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.is_open());
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    std::string Text(const std::filesystem::path &path) {
        std::ifstream in(path);
        REQUIRE(in.is_open());
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    nlohmann::ordered_json Metadata(const std::filesystem::path &path) {
        return nlohmann::ordered_json::parse(Text(path));
    }
} // namespace

TEST_CASE("RawRecorder preserves complete telegram bytes and records bad checksums") {
    TempDirectory dir;
    constexpr char payload[] = "sSN LMDscandata \0\xFF";
    auto scan = Record(std::string(payload, sizeof(payload) - 1), true, false);
    auto reply = Record("sRA SCdevicestate 1", false);
    const auto scan_length = scan.frame.data.size();
    const auto reply_length = reply.frame.data.size();
    auto expected = scan.frame.data;
    expected.insert(expected.end(), reply.frame.data.begin(), reply.frame.data.end());

    lms4xxx::RawRecorder recorder(dir.Config());
    REQUIRE(recorder.Start());
    CHECK_FALSE(Metadata(dir.Split("000.json"))["closed_cleanly"].get<bool>());
    recorder.OnFrame(std::move(scan));
    recorder.OnFrame(std::move(reply));
    recorder.Stop();
    recorder.Stop(); // idempotent

    CHECK_FALSE(recorder.HasFailed());
    CHECK(Bytes(dir.Split("000.bin")) == expected);
    const auto stats = recorder.GetStatistics();
    CHECK(stats.frames_queued == 2);
    CHECK(stats.frames_written == 2);
    CHECK(stats.frames_dropped == 0);
    CHECK(stats.scans_queued == 1);
    CHECK(stats.scans_written == 1);
    CHECK(stats.bytes_written == expected.size());
    CHECK(stats.files_created == 1);
    const std::string header = "offset,length,host_receive_monotonic_us,is_scan,checksum_valid,"
        "device_time_unix_us,clock_step_us,clock_quality_flags\n";
    CHECK(Text(dir.Split("000.csv")) == header +
        "0," + std::to_string(scan_length) + ",1000001,1,0,1700000000123456,-13,1025\n" +
        std::to_string(scan_length) + "," + std::to_string(reply_length) + ",1000001,0,1,0,0,0\n");
    const auto metadata = Metadata(dir.Split("000.json"));
    CHECK(metadata["format"] == "lms4xxx-cola-b");
    CHECK(metadata["format_version"] == 1);
    CHECK(metadata["instance_id"] == "unit");
    CHECK(metadata["session_timestamp"] == "20261030_120000");
    CHECK(metadata["split_index"] == 0);
    CHECK(metadata["metadata"] == dir.Config().metadata);
    CHECK(metadata["closed_cleanly"].get<bool>());
    CHECK(metadata["frames_total"] == 2);
    CHECK(metadata["scans_total"] == 1);
    CHECK(metadata["bytes_total"] == expected.size());
}

TEST_CASE("RawRecorder splits before a whole telegram and does not create an empty tail") {
    TempDirectory dir;
    auto first = Record("sSN LMDscandata first", true);
    auto second = Record("sRA reply", false);
    const auto expected_first = first.frame.data;
    const auto expected_second = second.frame.data;
    auto config = dir.Config();
    config.max_file_bytes = expected_first.size();
    lms4xxx::RawRecorder recorder(std::move(config));
    REQUIRE(recorder.Start());
    recorder.OnFrame(std::move(first));
    recorder.OnFrame(std::move(second));
    recorder.Stop();

    CHECK_FALSE(recorder.HasFailed());
    CHECK(Bytes(dir.Split("000.bin")) == expected_first);
    CHECK(Bytes(dir.Split("001.bin")) == expected_second);
    CHECK_FALSE(std::filesystem::exists(dir.Split("002.bin")));
    CHECK(Text(dir.Split("001.csv")).find("\n0," + std::to_string(expected_second.size()) + ",") != std::string::npos);
    CHECK(Metadata(dir.Split("000.json"))["closed_cleanly"].get<bool>());
    CHECK(Metadata(dir.Split("001.json"))["closed_cleanly"].get<bool>());
    CHECK(Metadata(dir.Split("001.json"))["split_index"] == 1);
    CHECK(Metadata(dir.Split("001.json"))["scans_total"] == 0);
    CHECK(recorder.GetStatistics().files_created == 2);
}

TEST_CASE("RawRecorder never truncates a telegram larger than the split limit") {
    TempDirectory dir;
    auto frame = Record("sSN LMDscandata oversized", true);
    const auto expected = frame.frame.data;
    auto config = dir.Config();
    config.max_file_bytes = 1;
    lms4xxx::RawRecorder recorder(std::move(config));
    REQUIRE(recorder.Start());
    recorder.OnFrame(std::move(frame));
    recorder.Stop();
    CHECK_FALSE(recorder.HasFailed());
    CHECK(Bytes(dir.Split("000.bin")) == expected);
}

TEST_CASE("RawRecorder refuses collisions for every sidecar and binary file") {
    TempDirectory dir;
    std::filesystem::path existing;
    SUBCASE("binary") { existing = dir.Split("000.bin"); }
    SUBCASE("CSV index") { existing = dir.Split("000.csv"); }
    SUBCASE("JSON metadata") { existing = dir.Split("000.json"); }
    { std::ofstream out(existing); out << "original recording"; }
    lms4xxx::RawRecorder recorder(dir.Config());
    CHECK_FALSE(recorder.Start());
    CHECK(recorder.HasFailed());
    recorder.Stop();
    CHECK(Text(existing) == "original recording");
}

TEST_CASE("RawRecorder failed append remains incomplete and accounts for the rejected frame") {
    TempDirectory dir;
    lms4xxx::RawRecorder recorder(dir.Config());
    REQUIRE(recorder.Start());
    recorder.OnFrame(lms4xxx::RawRecord{}); // no complete telegram
    recorder.Stop();
    CHECK(recorder.HasFailed());
    CHECK_FALSE(Metadata(dir.Split("000.json"))["closed_cleanly"].get<bool>());
    CHECK(recorder.GetStatistics().frames_queued == 1);
    CHECK(recorder.GetStatistics().frames_written == 0);
    CHECK(recorder.GetStatistics().frames_dropped == 1);
}
