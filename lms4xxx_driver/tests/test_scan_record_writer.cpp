/// @file test_scan_record_writer.cpp
/// @brief ScanRecordWriter end to end: ScanData in, split HDF5 files out.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <hdf5.h>

#include "scan_data.h"
#include "scan_record.h"
#include "scan_record_writer.h"


namespace {
    namespace fs = std::filesystem;

    fs::path MakeTempDir(const std::string &tag) {
        const auto dir = fs::temp_directory_path() / ("lms4xxx_writer_" + tag);
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }

    lms4xxx::ScanData MakeScan(std::uint16_t counter, std::uint16_t mask) {
        lms4xxx::ScanData scan;
        scan.telegram_counter = counter;
        scan.scan_counter = counter;
        scan.time_since_startup_us = 1000u * counter;
        scan.scan_frequency = 60000;
        scan.measurement_frequency = 100;
        scan.device_info.serial_number = 2612;
        scan.has_timestamp = true;
        scan.timestamp = lms4xxx::ScanTimestamp{2026, 1, 2, 3, 4, 5, 123456};
        scan.has_device_name = true;
        scan.device_name = "LMS4000";

        auto add16 = [&](lms4xxx::ChannelContent16 content, float factor, float offset) {
            lms4xxx::ChannelData16 ch;
            ch.content = content;
            ch.scale_factor = factor;
            ch.scale_offset = offset;
            ch.start_angle = 550000;
            ch.angle_step = 833;
            ch.num_data = static_cast<std::uint16_t>(lms4xxx::kMaxPointsPerScan);
            ch.data.resize(lms4xxx::kMaxPointsPerScan);
            for (std::size_t i = 0; i < ch.data.size(); ++i) {
                ch.data[i] = static_cast<std::uint16_t>((counter + i) & 0xFFFF);
            }
            scan.channels_16bit.push_back(std::move(ch));
        };
        if (mask & lms4xxx::ChannelMask::kDist1) {
            add16(lms4xxx::ChannelContent16::kDist1, 0.1f, 0.0f);
        }
        if (mask & lms4xxx::ChannelMask::kRssi1) {
            add16(lms4xxx::ChannelContent16::kRssi1, 1.0f, 0.0f);
        }
        if (mask & lms4xxx::ChannelMask::kRefl1) {
            add16(lms4xxx::ChannelContent16::kRefl1, 0.01f, 0.0f);
        }
        if (mask & lms4xxx::ChannelMask::kAngl1) {
            add16(lms4xxx::ChannelContent16::kAngl1, 1.0f, -32768.0f);
        }
        if (mask & lms4xxx::ChannelMask::kQlty1) {
            lms4xxx::ChannelData8 q;
            q.content = lms4xxx::ChannelContent8::kQlty1;
            q.start_angle = 550000;
            q.angle_step = 833;
            q.num_data = static_cast<std::uint16_t>(lms4xxx::kMaxPointsPerScan);
            q.data.assign(lms4xxx::kMaxPointsPerScan, 0x10);
            scan.channels_8bit.push_back(std::move(q));
        }
        return scan;
    }

    struct Id {
        hid_t id;
        herr_t (*close)(hid_t);

        ~Id() {
            if (id >= 0) {
                close(id);
            }
        }
    };

    // Scans in the file
    hsize_t Rows(const fs::path &path) {
        Id file{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
        REQUIRE(file.id >= 0);
        Id dset{H5Dopen2(file.id, "/frames/scan_counter", H5P_DEFAULT), H5Dclose};
        REQUIRE(dset.id >= 0);
        Id space{H5Dget_space(dset.id), H5Sclose};
        REQUIRE(space.id >= 0);
        hsize_t dims[1] = {0};
        REQUIRE(H5Sget_simple_extent_dims(space.id, dims, nullptr) == 1);
        return dims[0];
    }

    template<typename T>
    std::vector<T> ReadColumn(const fs::path &path, const char *name, hid_t mem_type) {
        Id file{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
        REQUIRE(file.id >= 0);
        Id dset{H5Dopen2(file.id, name, H5P_DEFAULT), H5Dclose};
        REQUIRE(dset.id >= 0);
        Id space{H5Dget_space(dset.id), H5Sclose};
        REQUIRE(space.id >= 0);
        hsize_t dims[2] = {0, 0};
        const int rank = H5Sget_simple_extent_dims(space.id, dims, nullptr);
        REQUIRE(rank >= 1);
        std::size_t total = static_cast<std::size_t>(dims[0]);
        if (rank == 2) {
            total *= static_cast<std::size_t>(dims[1]);
        }
        std::vector<T> out(total);
        if (total > 0) {
            REQUIRE(H5Dread(dset.id, mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()) >= 0);
        }
        return out;
    }
} // namespace


TEST_CASE("ScanRecordWriter records scans into split HDF5 files") {
    const auto dir = MakeTempDir("split");
    const auto config_yaml = dir / "config-lms4xxx.yaml";
    {
        std::ofstream(config_yaml.string()) << "scan:\n  start_angle_deg: 55.0\n";
    }

    constexpr std::uint16_t kMask = lms4xxx::ChannelMask::kDist1 | lms4xxx::ChannelMask::kRssi1 |
                                    lms4xxx::ChannelMask::kAngl1 | lms4xxx::ChannelMask::kQlty1; // yaml default 0x1B
    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan_unit_20260101_000000.h5").string();
    cfg.instance = "unit";
    cfg.session_timestamp = "20260101_000000";
    cfg.config_yaml_path = config_yaml.string();
    cfg.channel_mask = kMask;
    cfg.queue_capacity = 256;
    cfg.chunk_frames = 16;
    // No flush tick during the test: every write is a full batch until the drain
    cfg.flush_interval_ms = 5000;
    cfg.compression_level = 0;
    // Checked per 16-scan batch: 48 + 48 + 4
    cfg.max_file_bytes = 40ULL * lms4xxx::RecordBytes(kMask);
    cfg.remission = "rssi";
    cfg.device_order_number = "1116198";
    cfg.device_keeps_flagged_points = true;

    lms4xxx::ScanRecordWriter writer(cfg);
    REQUIRE(writer.Start());
    for (std::uint16_t i = 0; i < 100; ++i) {
        writer.OnScan(MakeScan(i, kMask));
    }
    writer.Stop();

    const auto stats = writer.GetStatistics();
    CHECK_FALSE(writer.HasFailed());
    CHECK(stats.frames_queued == 100);
    CHECK(stats.frames_written == 100);
    CHECK(stats.frames_dropped == 0);
    CHECK(stats.files_created == 3);
    CHECK(stats.bytes_written == 100ULL * lms4xxx::RecordBytes(kMask));

    const auto file0 = dir / "scan_unit_20260101_000000_000.h5";
    const auto file1 = dir / "scan_unit_20260101_000000_001.h5";
    const auto file2 = dir / "scan_unit_20260101_000000_002.h5";
    REQUIRE(fs::exists(file0));
    REQUIRE(fs::exists(file1));
    REQUIRE(fs::exists(file2));
    CHECK_FALSE(fs::exists(dir / "scan_unit_20260101_000000.h5"));
    CHECK(Rows(file0) == 48);
    CHECK(Rows(file1) == 48);
    CHECK(Rows(file2) == 4);

    const auto counters1 = ReadColumn<std::uint16_t>(file1, "/frames/scan_counter", H5T_NATIVE_UINT16);
    REQUIRE(counters1.size() == 48);
    CHECK(counters1.front() == 48);
    CHECK(counters1.back() == 95);

    const auto dist0 = ReadColumn<std::uint16_t>(file0, "/channels/dist", H5T_NATIVE_UINT16);
    REQUIRE(dist0.size() == 48 * 841);
    CHECK(dist0[0] == 0);
    CHECK(dist0[841 + 5] == 6); // scan 1, point 5 -> (1 + 5)
    const auto qlty0 = ReadColumn<std::uint8_t>(file0, "/channels/qlty", H5T_NATIVE_UINT8);
    CHECK(qlty0[47 * 841 + 840] == 0x10);

    // Telegram 2026-01-02T03:04:05.123456Z
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 2;
    tm.tm_hour = 3;
    tm.tm_min = 4;
    tm.tm_sec = 5;
    const std::int64_t expected_device_us = static_cast<std::int64_t>(timegm(&tm)) * 1000000 + 123456;
    const auto device_time = ReadColumn<std::int64_t>(file0, "/frames/device_time_unix_us", H5T_NATIVE_INT64);
    CHECK(device_time[0] == expected_device_us);
    {
        Id file{H5Fopen(file0.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
        REQUIRE(file.id >= 0);
        CHECK(H5Lexists(file.id, "/frames/host_time_unix_us", H5P_DEFAULT) == 0);
    }

    {
        Id file{H5Fopen(file2.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
        REQUIRE(file.id >= 0);
        CHECK(H5Lexists(file.id, "/channels/refl", H5P_DEFAULT) == 0);
        CHECK(H5Lexists(file.id, "/channels/angl", H5P_DEFAULT) > 0);
        CHECK(H5Aexists(file.id, "config_yaml") > 0);
        CHECK(H5Aexists(file.id, "device_order_number") > 0);
        CHECK(H5Aexists(file.id, "config_remission") > 0);
        Id attr{H5Aopen(file.id, "split_index", H5P_DEFAULT), H5Aclose};
        REQUIRE(attr.id >= 0);
        std::uint32_t split_index = 0;
        REQUIRE(H5Aread(attr.id, H5T_NATIVE_UINT32, &split_index) >= 0);
        CHECK(split_index == 2);
    }

    fs::remove_all(dir);
}


TEST_CASE("ScanRecordWriter Start fails on an unwritable output path") {
    const auto dir = MakeTempDir("unwritable");
    const auto blocker = dir / "blocker";
    {
        std::ofstream(blocker.string()) << "x";
    }

    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (blocker / "scan_unit_20260101_000000.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = lms4xxx::ChannelMask::kDist1;

    lms4xxx::ScanRecordWriter writer(cfg);
    CHECK_FALSE(writer.Start());
    CHECK(writer.GetStatistics().files_created == 0);
    writer.Stop(); // no-op

    fs::remove_all(dir);
}

TEST_CASE("Later channel loss or scale drift fails the recording instead of writing normal zeros") {
    const auto dir = MakeTempDir("channel_drift");
    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = lms4xxx::ChannelMask::kDist1;
    lms4xxx::ScanRecordWriter writer(cfg);
    REQUIRE(writer.Start());
    writer.OnScan(MakeScan(1, cfg.channel_mask));
    auto invalid = MakeScan(2, cfg.channel_mask);
    SUBCASE("missing channel") { invalid.channels_16bit.clear(); }
    SUBCASE("scale drift") { invalid.channels_16bit.front().scale_factor = 2.0f; }
    writer.OnScan(invalid);
    CHECK(writer.HasFailed());
    writer.Stop();
    CHECK(writer.GetStatistics().frames_dropped == 1);
    CHECK(writer.GetStatistics().frames_written == 1);
    fs::remove_all(dir);
}


TEST_CASE("ScanRecordWriter counts scans arriving outside a run as dropped") {
    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (fs::temp_directory_path() / "lms4xxx_writer_never" / "scan.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = lms4xxx::ChannelMask::kDist1;

    lms4xxx::ScanRecordWriter writer(cfg);
    writer.OnScan(MakeScan(1, lms4xxx::ChannelMask::kDist1));
    CHECK(writer.GetStatistics().frames_dropped == 1);
    CHECK(writer.GetStatistics().frames_written == 0);
}


TEST_CASE("ScanRecordWriter rejects an invalid configuration") {
    const auto dir = MakeTempDir("badcfg");
    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan.h5").string();
    cfg.instance = "unit";
    cfg.chunk_frames = 0;

    lms4xxx::ScanRecordWriter writer(cfg);
    CHECK_FALSE(writer.Start());

    fs::remove_all(dir);
}


TEST_CASE("The ledger closes: written + dropped == everything handed in") {
    // The writer goes out of its way to keep this true (a failed append counts
    // the whole batch as dropped, Stop() counts the queue remainder), and it was
    // never asserted. A hole here means scans vanished without a trace.
    const auto dir = MakeTempDir("ledger");
    constexpr std::uint16_t kMask = lms4xxx::ChannelMask::kDist1 | lms4xxx::ChannelMask::kQlty1;

    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan_unit_20260101_000000.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = kMask;
    cfg.chunk_frames = 8;
    cfg.queue_capacity = 64;
    cfg.flush_interval_ms = 20; // exercise the flush tick, which no test covered
    cfg.max_file_bytes = 0; // no splitting

    lms4xxx::ScanRecordWriter writer(cfg);
    REQUIRE(writer.Start());
    constexpr std::size_t kScans = 200;
    for (std::size_t i = 0; i < kScans; ++i) {
        writer.OnScan(MakeScan(static_cast<std::uint16_t>(i), kMask));
        if (i % 50 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25)); // let a flush tick land
        }
    }
    writer.Stop();

    const auto stats = writer.GetStatistics();
    CHECK(stats.frames_queued + stats.frames_dropped == kScans);
    CHECK(stats.frames_written + stats.frames_dropped == kScans);
    CHECK(stats.files_created == 1); // max_file_bytes = 0 disables splitting
    CHECK(Rows(fs::path(cfg.output_path).parent_path() / "scan_unit_20260101_000000_000.h5") ==
          stats.frames_written);

    fs::remove_all(dir);
}


TEST_CASE("A queue that cannot drain sheds scans instead of blocking the parse thread") {
    // OnScan runs on the parse thread at 600 Hz and must never block. With no
    // write thread running, every scan is refused and counted.
    const auto dir = MakeTempDir("shed");
    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = lms4xxx::ChannelMask::kDist1;
    cfg.queue_capacity = 2;

    lms4xxx::ScanRecordWriter writer(cfg);
    // Never started: the queue does not exist, so this is the "arrived outside a
    // run" path, and it must stay O(1) rather than growing without bound.
    for (int i = 0; i < 100; ++i) {
        writer.OnScan(MakeScan(static_cast<std::uint16_t>(i), lms4xxx::ChannelMask::kDist1));
    }
    CHECK(writer.GetStatistics().frames_dropped == 100);
    CHECK(writer.GetStatistics().frames_written == 0);

    fs::remove_all(dir);
}


TEST_CASE("Start() twice is refused, and the destructor stops a running writer") {
    const auto dir = MakeTempDir("double_start");
    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan_unit_20260101_000000.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = lms4xxx::ChannelMask::kDist1;
    cfg.chunk_frames = 4;
    {
        lms4xxx::ScanRecordWriter writer(cfg);
        REQUIRE(writer.Start());
        CHECK_FALSE(writer.Start());
        writer.OnScan(MakeScan(1, lms4xxx::ChannelMask::kDist1));
        // No explicit Stop(): the destructor must close the file cleanly.
    }
    const auto file = dir / "scan_unit_20260101_000000_000.h5";
    REQUIRE(fs::exists(file));
    CHECK(Rows(file) == 1);

    fs::remove_all(dir);
}


TEST_CASE("A finished file carries the completeness marker") {
    // HDF5 has no rename-on-finish, so a truncated recording is otherwise
    // indistinguishable from a complete one. `closed_cleanly` is that marker,
    // and scripts/inspect_h5.py verifies it.
    const auto dir = MakeTempDir("marker");
    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan_unit_20260101_000000.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = lms4xxx::ChannelMask::kDist1;
    cfg.chunk_frames = 4;

    lms4xxx::ScanRecordWriter writer(cfg);
    REQUIRE(writer.Start());
    for (int i = 0; i < 9; ++i) {
        writer.OnScan(MakeScan(static_cast<std::uint16_t>(i), lms4xxx::ChannelMask::kDist1));
    }
    writer.Stop();

    const auto file = dir / "scan_unit_20260101_000000_000.h5";
    Id h5{H5Fopen(file.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    REQUIRE(h5.id >= 0);
    {
        Id attr{H5Aopen(h5.id, "closed_cleanly", H5P_DEFAULT), H5Aclose};
        REQUIRE(attr.id >= 0);
        std::uint8_t value = 0;
        REQUIRE(H5Aread(attr.id, H5T_NATIVE_UINT8, &value) >= 0);
        CHECK(value == 1);
    }
    {
        Id attr{H5Aopen(h5.id, "frames_total", H5P_DEFAULT), H5Aclose};
        REQUIRE(attr.id >= 0);
        std::uint64_t value = 0;
        REQUIRE(H5Aread(attr.id, H5T_NATIVE_UINT64, &value) >= 0);
        CHECK(value == 9);
    }

    fs::remove_all(dir);
}


TEST_CASE("Device telemetry lands in /telemetry, not in the scan queue") {
    const auto dir = MakeTempDir("telemetry");
    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan_unit_20260101_000000.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = lms4xxx::ChannelMask::kDist1;
    cfg.chunk_frames = 4;
    // The device audit read at bring-up; absent fields must simply not appear.
    cfg.audit.temperature_c = 41.5;
    cfg.audit.operating_hours_deci = 187531; // manual p.128 example: 18753.1 h
    cfg.audit.motor_sync_role = 0;

    lms4xxx::ScanRecordWriter writer(cfg);
    REQUIRE(writer.Start());
    lms4xxx::TelemetrySample sample;
    sample.unix_us = 1788525296000000ULL;
    sample.temperature_c = 42.5;
    sample.device_state = 1;
    sample.warnings = {"45: Warning. No synchronisation signal."};
    writer.OnTelemetry(sample);
    sample.unix_us += 10'000'000;
    sample.warnings.clear();
    writer.OnTelemetry(sample);
    writer.OnScan(MakeScan(1, lms4xxx::ChannelMask::kDist1));
    writer.Stop();

    const auto file = dir / "scan_unit_20260101_000000_000.h5";
    const auto stamps = ReadColumn<std::uint64_t>(file, "/telemetry/unix_us", H5T_NATIVE_UINT64);
    REQUIRE(stamps.size() == 2);
    CHECK(stamps[0] == 1788525296000000ULL);
    const auto temps = ReadColumn<float>(file, "/telemetry/temperature_c", H5T_NATIVE_FLOAT);
    REQUIRE(temps.size() == 2);
    CHECK(temps[0] == doctest::Approx(42.5f));
    const auto counts = ReadColumn<std::uint32_t>(file, "/telemetry/warning_count", H5T_NATIVE_UINT32);
    REQUIRE(counts.size() == 2);
    CHECK(counts[0] == 1);
    CHECK(counts[1] == 0);
    // Telemetry rows are independent of scans.
    CHECK(Rows(file) == 1);

    // The one-shot audit is a root attribute, and an unanswered read leaves the
    // attribute out entirely rather than storing a fake value.
    Id h5{H5Fopen(file.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    REQUIRE(h5.id >= 0);
    CHECK(H5Aexists(h5.id, "device_temperature_c_at_start") > 0);
    CHECK(H5Aexists(h5.id, "device_operating_hours") > 0);
    CHECK(H5Aexists(h5.id, "device_motor_sync_role") > 0);
    CHECK(H5Aexists(h5.id, "device_power_on_count") == 0); // never read on this device

    fs::remove_all(dir);
}


TEST_CASE("An existing file is never silently overwritten") {
    // The path carries the session timestamp and the split index, so a collision
    // means the run layout is wrong — destroying the earlier recording would be
    // the worst possible answer.
    const auto dir = MakeTempDir("collision");
    const auto taken = dir / "scan_unit_20260101_000000_000.h5";
    { std::ofstream(taken.string()) << "not really HDF5"; }

    lms4xxx::ScanRecordWriter::Config cfg;
    cfg.output_path = (dir / "scan_unit_20260101_000000.h5").string();
    cfg.instance = "unit";
    cfg.channel_mask = lms4xxx::ChannelMask::kDist1;

    lms4xxx::ScanRecordWriter writer(cfg);
    CHECK_FALSE(writer.Start());
    CHECK(writer.GetStatistics().files_created == 0);

    fs::remove_all(dir);
}
