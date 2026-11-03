#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "ebus_driver/raw_format.h"
#include "ebus_driver/raw_recorder.h"
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

namespace {
struct Temp {
    std::filesystem::path root;
    Temp() {
        char pattern[] = "/tmp/ebus_raw_test_XXXXXX";
        const char *created = ::mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
        root = created;
    }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
std::vector<unsigned char> Read(const std::filesystem::path &file) {
    std::ifstream input(file, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::uint64_t LE(const std::vector<unsigned char> &v, std::size_t at, std::size_t bytes) {
    REQUIRE(at + bytes <= v.size());
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < bytes; ++i) out |= std::uint64_t(v[at + i]) << (8 * i);
    return out;
}
// Independent bitwise CRC, also used for on-disk boundaries below.
std::uint32_t CRC(const unsigned char *bytes, std::size_t count) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82f63b78u : 0);
    }
    return ~crc;
}
std::vector<nlohmann::json> Events(const std::filesystem::path &directory) {
    std::ifstream input(directory / "events.jsonl");
    REQUIRE(input.good());
    std::vector<nlohmann::json> events;
    for (std::string line; std::getline(input, line);) events.push_back(nlohmann::json::parse(line));
    return events;
}
// Bound a real regular-file write so the kernel first accepts a prefix and
// then returns EFBIG. Restore the process limit and signal handler before any
// recorder finalization; no production failure-injection seam is needed.
struct FileSizeLimit {
    struct rlimit previous{};
    struct sigaction previous_action{};
    explicit FileSizeLimit(std::uintmax_t bytes) {
        if (::getrlimit(RLIMIT_FSIZE, &previous) != 0 ||
            (previous.rlim_max != RLIM_INFINITY && bytes > previous.rlim_max))
            throw std::runtime_error("cannot set test file-size limit");
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        ::sigemptyset(&ignored.sa_mask);
        if (::sigaction(SIGXFSZ, &ignored, &previous_action) != 0)
            throw std::runtime_error("cannot ignore test SIGXFSZ");
        const struct rlimit limited{static_cast<rlim_t>(bytes), previous.rlim_max};
        if (::setrlimit(RLIMIT_FSIZE, &limited) != 0) {
            ::sigaction(SIGXFSZ, &previous_action, nullptr);
            throw std::runtime_error("cannot set test file-size limit");
        }
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        ::sigaction(SIGXFSZ, &previous_action, nullptr);
    }
};
}

TEST_CASE("CRC32C known vector and empty buffer") {
    CHECK(ebus_driver::raw::Crc32c("123456789", 9) == 0xe3069283u);
    CHECK(ebus_driver::raw::Crc32c(nullptr, 0) == 0u);
}

TEST_CASE("raw preserves SDK bytes, flags and global sequence across rotation") {
    Temp temp;
    ebus_driver::RecorderOptions options;
    options.directory = temp.root / "camera";
    options.segment_records = 1;
    options.segment_bytes = 256; // The second oversized record must remain whole.
    options.flush_bytes = 1;
    ebus_driver::RawRecorder writer(options);
    writer.Open({{"id", "test-camera"}, {"recording_format", "ebus-raw-v1"}});
    const std::vector<ebus_driver::RawBuffer> buffers{
        {{{"valid_frame", true}, {"block_id", 65535}, {"image", {{"padding_x", 3}, {"padding_y", 7}}}},
         {0, 255, 128, 7, 0, 9}},
        {{{"valid_frame", false}, {"operation_ok", false}, {"operation_result", 17}},
         std::vector<std::uint8_t>(300, 0xa5)},
        {{{"valid_frame", true}, {"block_id", 1}}, {4, 3, 2, 1}}};
    for (const auto &buffer : buffers) writer.Append(buffer);
    CHECK(writer.RecordsWritten() == 3);
    CHECK(writer.FramesWritten() == 2);
    writer.Close(true, "test complete");
    writer.Close(true, "idempotent");
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        const auto file = options.directory / ("seg_0000" + std::to_string(i + 1) + ".raw");
        const auto bytes = Read(file);
        REQUIRE(bytes.size() >= 112);
        CHECK(std::string(bytes.begin(), bytes.begin() + 7) == "EBUSRAW");
        CHECK(bytes[7] == 0);
        CHECK(LE(bytes, 8, 2) == 1);
        CHECK(LE(bytes, 10, 2) == 64);
        CHECK(LE(bytes, 24, 8) == i + 1);
        CHECK(LE(bytes, 60, 4) == CRC(bytes.data(), 60));
        CHECK(std::string(bytes.begin() + 64, bytes.begin() + 68) == "EBFR");
        CHECK(LE(bytes, 68, 4) == 48);
        CHECK(LE(bytes, 88, 8) == i);
        CHECK(LE(bytes, 104, 4) == CRC(bytes.data() + 64, 40));
        const auto metadata_size = LE(bytes, 72, 4);
        const auto payload_size = LE(bytes, 80, 8);
        REQUIRE(bytes.size() == 112 + metadata_size + payload_size);
        CHECK(LE(bytes, 96, 4) == CRC(bytes.data() + 112, metadata_size));
        CHECK(LE(bytes, 100, 4) == CRC(bytes.data() + 112 + metadata_size, payload_size));
        const std::string metadata(bytes.begin() + 112, bytes.begin() + 112 + metadata_size);
        CHECK(nlohmann::json::parse(metadata) == buffers[i].metadata);
        const std::vector<std::uint8_t> payload(bytes.begin() + 112 + metadata_size, bytes.end());
        CHECK(payload == buffers[i].payload);
    }
    const auto events = Events(options.directory);
    REQUIRE(!events.empty());
    const auto &end = events.back();
    CHECK(end.at("event") == "session_end");
    CHECK(end.at("complete") == true);
    CHECK(end.at("records_written") == 3);
    CHECK(end.at("valid_frames_written") == 2);
    CHECK(end.at("segments_created") == 3);
}

TEST_CASE("recording never overwrites an existing camera directory") {
    Temp temp;
    ebus_driver::RecorderOptions options;
    options.directory = temp.root / "camera";
    std::filesystem::create_directory(options.directory);
    { std::ofstream file(options.directory / "seg_00001.raw"); file << "previous data"; }
    ebus_driver::RawRecorder writer(options);
    CHECK_THROWS(writer.Open(nlohmann::json::object()));
    const auto bytes = Read(options.directory / "seg_00001.raw");
    CHECK(std::string(bytes.begin(), bytes.end()) == "previous data");
}

TEST_CASE("failure rejects later appends and finalization reports incomplete") {
    Temp temp;
    ebus_driver::RecorderOptions options;
    options.directory = temp.root / "camera";
    ebus_driver::RawRecorder writer(options);
    writer.Open(nlohmann::json::object());
    writer.Append({{{"valid_frame", true}}, {1, 2, 3}});
    CHECK_THROWS(writer.Append({nlohmann::json::array(), {4}}));
    CHECK(writer.Failed());
    CHECK_THROWS(writer.Append({{{"valid_frame", true}}, {5}}));
    writer.Close(true, "caller must not override the failure latch");
    CHECK(Events(options.directory).back().at("complete") == false);
    CHECK(writer.RecordsWritten() == 1);
}

TEST_CASE("partial raw write rolls back without advancing record counts") {
    Temp temp;
    ebus_driver::RecorderOptions options;
    options.directory = temp.root / "camera";
    ebus_driver::RawRecorder writer(options);
    writer.Open(nlohmann::json::object());
    writer.Append({{{"valid_frame", true}}, {1, 2, 3}});
    const auto file = options.directory / "seg_00001.raw";
    const auto original = Read(file);
    {
        FileSizeLimit limit(original.size() + 8);
        CHECK_THROWS(writer.Append({{{"valid_frame", true}}, std::vector<std::uint8_t>(200, 7)}));
    }
    CHECK(writer.Failed());
    CHECK(writer.RecordsWritten() == 1);
    CHECK(writer.FramesWritten() == 1);
    CHECK(Read(file) == original);
    writer.Close(true, "write failure remains incomplete after rollback");
    const auto events = Events(options.directory);
    REQUIRE(!events.empty());
    CHECK(events.back().at("complete") == false);
    CHECK(events.back().at("records_written") == 1);
}

TEST_CASE("partial event write retains parseable prefix and incomplete finalization") {
    Temp temp;
    ebus_driver::RecorderOptions options;
    options.directory = temp.root / "camera";
    ebus_driver::RawRecorder writer(options);
    writer.Open(nlohmann::json::object());
    const auto file = options.directory / "events.jsonl";
    const auto original = Read(file);
    {
        FileSizeLimit limit(original.size() + 8);
        CHECK_THROWS(writer.Event({{"event", "test"}, {"large", std::string(200, 'x')}}));
    }
    CHECK(writer.Failed());
    CHECK(Read(file) == original);
    writer.Close(true, "event failure remains incomplete after rollback");
    const auto events = Events(options.directory);
    REQUIRE(!events.empty());
    CHECK(events.back().at("complete") == false);
}

TEST_CASE("failed exclusive segment creation does not count or overwrite that path") {
    Temp temp;
    ebus_driver::RecorderOptions options;
    options.directory = temp.root / "camera";
    options.segment_records = 1;
    ebus_driver::RawRecorder writer(options);
    writer.Open(nlohmann::json::object());
    writer.Append({{{"valid_frame", true}}, {1}});
    const auto occupied = options.directory / "seg_00002.raw";
    { std::ofstream file(occupied); file << "do not overwrite"; }
    CHECK_THROWS(writer.Append({{{"valid_frame", true}}, {2}}));
    writer.Close(true, "rotation failed");
    const auto bytes = Read(occupied);
    CHECK(std::string(bytes.begin(), bytes.end()) == "do not overwrite");
    CHECK(writer.Summary().at("segments_created") == 1);
    CHECK(writer.RecordsWritten() == 1);
    CHECK(Events(options.directory).back().at("complete") == false);
}
