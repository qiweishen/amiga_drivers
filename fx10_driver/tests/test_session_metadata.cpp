#include "session_metadata.h"
#include <doctest/doctest.h>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace {
    std::filesystem::path MetadataTestDir() {
        static std::atomic<unsigned> serial{0};
        auto path = std::filesystem::temp_directory_path() /
            ("fx10_metadata_" + std::to_string(::getpid()) + "_" + std::to_string(serial++));
        std::filesystem::create_directories(path);
        return path;
    }
}

TEST_CASE("Metadata: UnknownAndRegressedCountersCannotBecomeValidDeltas") {
    fx10::DeviceTelemetry sample;
    auto row = fx10::BuildTelemetryJson(sample, "start");
    CHECK(row["missed_triggers"]["raw"].is_null());
    CHECK(row["missed_triggers"]["delta"].is_null());
    CHECK(row["temp"]["proc_pcb"]["status"] == "unavailable");
    sample.missed_baseline = 10;
    sample.missed_raw = 12;
    CHECK_FALSE(fx10::MissedTriggerDelta(sample).has_value()); // binding unknown
    sample.counter_binding_verified = true;
    CHECK(fx10::MissedTriggerDelta(sample).value() == 2);
    sample.missed_raw = 3; // reset/wrap: no invented correction
    CHECK_FALSE(fx10::MissedTriggerDelta(sample).has_value());
    sample.missed_raw = 15;
    sample.counter_regressed = true; // remains unknown after exceeding the old baseline
    CHECK_FALSE(fx10::MissedTriggerDelta(sample).has_value());
    sample.temp_pcb_c = std::numeric_limits<double>::quiet_NaN();
    row = fx10::BuildTelemetryJson(sample, "stop");
    CHECK(row["temp"]["proc_pcb"]["value"].is_null());
    CHECK(row["temp"]["proc_pcb"]["status"] == "unavailable");
}

TEST_CASE("Metadata: JsonIntegersAndPixelContractRetainTheirMeaning") {
    fx10::DeviceTelemetry sample;
    sample.host_realtime_ns = 1800000000000000001ULL;
    sample.missed_raw = 9007199254740993LL;
    const auto row = nlohmann::json::parse(fx10::BuildTelemetryJson(sample, "periodic").dump());
    CHECK(row["hrt"].get<std::uint64_t>() == sample.host_realtime_ns);
    CHECK(row["missed_triggers"]["raw"].get<std::int64_t>() == *sample.missed_raw);
    const auto contract = fx10::RecordingContract("Mono12Packed");
    CHECK(contract["packed_pixels_unpacked"] == true);
    CHECK(contract["storage_bytes_per_pixel"] == 2);
    CHECK(contract["sdk_payload_byte_reconstruction"] == false);
    CHECK(fx10::RecordingContract("unknown")["storage_bytes_per_pixel"].is_null());
}

TEST_CASE("Metadata: AtomicPublishNeverClobbersExistingSnapshot") {
    const auto path = MetadataTestDir() / "device.json";
    fx10::PublishMetadata(path, {{"serial", "00123"}});
    CHECK_THROWS_AS(fx10::PublishMetadata(path, {{"serial", "other"}}), fx10::MetadataError);
    std::ifstream in(path);
    nlohmann::json value;
    in >> value;
    CHECK(value["serial"] == "00123");
    CHECK_FALSE(std::filesystem::exists(path.string() + ".part"));
}

TEST_CASE("Metadata: FailedPartialAppendRetainsPrefixAndCannotBeRetried") {
    const auto path = MetadataTestDir() / "telemetry.jsonl";
    fx10::JsonlFile out;
    out.Open(path);
    out.Append({{"phase", "start"}});
    const auto committed_size = std::filesystem::file_size(path);
    int calls = 0;
    out.SetWriteHookForTest([&](int fd, const void *data, std::size_t size) -> ssize_t {
        if (++calls == 1) return ::write(fd, data, size > 3 ? 3 : size);
        errno = ENOSPC;
        return -1;
    });
    CHECK_THROWS_AS(out.Append({{"phase", "periodic"}}), fx10::MetadataError);
    CHECK(out.Failed());
    CHECK(std::filesystem::file_size(path) == committed_size);
    out.SetWriteHookForTest({});
    CHECK_THROWS_AS(out.Append({{"phase", "stop"}}), fx10::MetadataError);
    CHECK(std::filesystem::file_size(path) == committed_size);
    out.Close();
}

TEST_CASE("Metadata: ZeroProgressWriteFailsWithoutSpinning") {
    fx10::JsonlFile out;
    out.Open(MetadataTestDir() / "segments.jsonl");
    out.SetWriteHookForTest([](int, const void *, std::size_t) -> ssize_t { return 0; });
    CHECK_THROWS_AS(out.Append({{"segment", "segment_0001"}}), fx10::MetadataError);
    CHECK(out.Failed());
}
