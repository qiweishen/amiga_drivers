/// @file test_scan_h5_file.cpp
/// @brief ScanH5File round trips, read back through the plain HDF5 C API.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <hdf5.h>

#include "scan_h5_file.h"
#include "scan_record.h"


namespace {
    namespace fs = std::filesystem;

    fs::path MakeTempDir(const std::string &tag) {
        const auto dir = fs::temp_directory_path() / ("lms4xxx_h5_" + tag);
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }

    // Every channel value depends on (scan, point)
    lms4xxx::ScanRecord MakeRecord(std::uint32_t index) {
        lms4xxx::ScanRecord r{};
        r.meta.scan_counter = static_cast<std::uint16_t>(index);
        r.meta.telegram_counter = static_cast<std::uint16_t>(index + 1);
        r.meta.num_points = static_cast<std::uint16_t>(lms4xxx::kMaxPointsPerScan);
        r.meta.start_angle = 550000;
        r.meta.angle_step = 833;
        r.meta.scan_frequency = 60000;
        r.meta.device_time_unix_us = 1700000000000000LL + index;
        r.meta.host_receive_monotonic_us = 10000000ULL + index;
        r.meta.clock_step_us = -20644;
        r.meta.clock_quality_flags = lms4xxx::ClockFlag::kAssessed | lms4xxx::ClockFlag::kBackwardUtc;
        r.meta.has_device_name = 1;
        std::strncpy(r.meta.device_name, "unit-test", sizeof(r.meta.device_name) - 1);
        for (std::size_t i = 0; i < lms4xxx::kMaxPointsPerScan; ++i) {
            r.dist[i] = static_cast<std::uint16_t>((index * 7 + i) & 0xFFFF);
            r.rssi[i] = static_cast<std::uint16_t>(i);
            r.refl[i] = static_cast<std::uint16_t>(2 * i);
            r.angl[i] = static_cast<std::uint16_t>(32768 + (i % 5));
            r.qlty[i] = 0x10;
        }
        return r;
    }

    lms4xxx::ScanH5File::Options BaseOptions(const fs::path &path) {
        lms4xxx::ScanH5File::Options opts;
        opts.path = path.string();
        opts.instance = "unit";
        opts.session_timestamp = "20260101_000000";
        opts.config_yaml = "lidar: []\n";
        opts.remission = "rssi";
        opts.device_firmware = "LMS41xxx 1.6.0.0R";
        opts.device_order_number = "1116198";
        opts.device_type = "LMS4124R-13000S01";
        opts.device_name = "unit-test";
        opts.device_keeps_flagged_points = true;
        opts.channel_mask = lms4xxx::ChannelMask::kAll;
        opts.split_index = 0;
        opts.chunk_frames = 8;
        opts.compression_level = 0;
        opts.swmr = false;
        opts.start_angle_deg = 55.0;
        opts.stop_angle_deg = 125.0;
        opts.angle_step_deg = 0.0833;
        opts.output_rate = 1;
        return opts;
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

    std::vector<hsize_t> Dims(hid_t file, const char *path) {
        Id dset{H5Dopen2(file, path, H5P_DEFAULT), H5Dclose};
        REQUIRE(dset.id >= 0);
        Id space{H5Dget_space(dset.id), H5Sclose};
        REQUIRE(space.id >= 0);
        const int rank = H5Sget_simple_extent_ndims(space.id);
        REQUIRE(rank >= 1);
        std::vector<hsize_t> dims(static_cast<std::size_t>(rank));
        REQUIRE(H5Sget_simple_extent_dims(space.id, dims.data(), nullptr) == rank);
        return dims;
    }

    template<typename T>
    std::vector<T> ReadAll(hid_t file, const char *path, hid_t mem_type) {
        const auto dims = Dims(file, path);
        std::size_t total = 1;
        for (const auto d: dims) {
            total *= static_cast<std::size_t>(d);
        }
        std::vector<T> out(total);
        Id dset{H5Dopen2(file, path, H5P_DEFAULT), H5Dclose};
        REQUIRE(dset.id >= 0);
        if (total > 0) {
            REQUIRE(H5Dread(dset.id, mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()) >= 0);
        }
        return out;
    }

    std::string ReadStringAttr(hid_t loc, const char *name) {
        Id attr{H5Aopen(loc, name, H5P_DEFAULT), H5Aclose};
        REQUIRE(attr.id >= 0);
        Id type{H5Aget_type(attr.id), H5Tclose};
        REQUIRE(type.id >= 0);
        REQUIRE(H5Tis_variable_str(type.id) > 0);
        char *text = nullptr;
        REQUIRE(H5Aread(attr.id, type.id, &text) >= 0);
        std::string value = text != nullptr ? text : "";
        H5free_memory(text);
        return value;
    }

    template<typename T>
    T ReadScalarAttr(hid_t loc, const char *name, hid_t mem_type) {
        Id attr{H5Aopen(loc, name, H5P_DEFAULT), H5Aclose};
        REQUIRE(attr.id >= 0);
        T value{};
        REQUIRE(H5Aread(attr.id, mem_type, &value) >= 0);
        return value;
    }

    std::vector<std::string> ReadFixedStrings(hid_t file, const char *path) {
        const auto dims = Dims(file, path);
        REQUIRE(dims.size() == 1);
        Id dset{H5Dopen2(file, path, H5P_DEFAULT), H5Dclose};
        REQUIRE(dset.id >= 0);
        Id type{H5Dget_type(dset.id), H5Tclose};
        REQUIRE(type.id >= 0);
        const std::size_t width = H5Tget_size(type.id);
        REQUIRE(width == 16);
        std::vector<char> buf(static_cast<std::size_t>(dims[0]) * width, '\0');
        if (!buf.empty()) {
            REQUIRE(H5Dread(dset.id, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data()) >= 0);
        }
        std::vector<std::string> out;
        for (hsize_t i = 0; i < dims[0]; ++i) {
            const char *p = buf.data() + static_cast<std::size_t>(i) * width;
            out.emplace_back(p, strnlen(p, width));
        }
        return out;
    }
} // namespace


TEST_CASE("ScanH5File writes the lms4xxx-h5 layout and reads back through the C API") {
    const auto dir = MakeTempDir("layout");
    const auto path = dir / "scan_unit_20260101_000000_000.h5";
    auto opts = BaseOptions(path);

    std::vector<lms4xxx::ScanRecord> records;
    for (std::uint32_t i = 0; i < 20; ++i) {
        records.push_back(MakeRecord(i));
    }

    {
        lms4xxx::ScanH5File file;
        REQUIRE_MESSAGE(file.Open(opts), file.LastError());
        CHECK(file.IsOpen());
        CHECK_FALSE(file.CompressionActive());
        REQUIRE_MESSAGE(file.Append(records.data(), 8), file.LastError()); // one full chunk
        REQUIRE_MESSAGE(file.Append(records.data() + 8, 5), file.LastError()); // partial chunk
        REQUIRE_MESSAGE(file.Flush(), file.LastError());
        REQUIRE_MESSAGE(file.Append(records.data() + 13, 7), file.LastError());
        CHECK(file.FramesWritten() == 20);
        file.Close();
        CHECK_FALSE(file.IsOpen());
    }

    Id file{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    REQUIRE(file.id >= 0);

    CHECK(ReadStringAttr(file.id, "format") == "lms4xxx-h5");
    CHECK(ReadScalarAttr<std::uint32_t>(file.id, "format_version", H5T_NATIVE_UINT32) == 4u);
    CHECK(ReadAll<std::uint64_t>(file.id, "/frames/host_receive_monotonic_us", H5T_NATIVE_UINT64)[0] == 10000000ULL);
    CHECK(ReadAll<std::int64_t>(file.id, "/frames/clock_step_us", H5T_NATIVE_INT64)[0] == -20644);
    CHECK(ReadAll<std::uint16_t>(file.id, "/frames/clock_quality_flags", H5T_NATIVE_UINT16)[0] == 17);
    CHECK(ReadStringAttr(file.id, "config_remission") == "rssi");
    CHECK(ReadStringAttr(file.id, "device_order_number") == "1116198");
    CHECK(ReadStringAttr(file.id, "device_firmware") == "LMS41xxx 1.6.0.0R");
    CHECK(ReadScalarAttr<std::uint8_t>(file.id, "device_keeps_flagged_points", H5T_NATIVE_UINT8) == 1);
    CHECK(ReadStringAttr(file.id, "instance_id") == "unit");
    CHECK(ReadStringAttr(file.id, "session_timestamp") == "20260101_000000");
    CHECK(ReadScalarAttr<std::uint16_t>(file.id, "channel_mask", H5T_NATIVE_UINT16) == lms4xxx::ChannelMask::kAll);
    CHECK(ReadStringAttr(file.id, "channels") == "dist,rssi,refl,angl,qlty");
    CHECK(ReadScalarAttr<std::uint16_t>(file.id, "points_per_scan", H5T_NATIVE_UINT16) == 841);
    CHECK(ReadScalarAttr<double>(file.id, "config_start_angle_deg", H5T_NATIVE_DOUBLE) == doctest::Approx(55.0));
    CHECK(ReadStringAttr(file.id, "compression") == "none");
    CHECK(ReadStringAttr(file.id, "config_yaml") == "lidar: []\n");

    CHECK(Dims(file.id, "/frames/scan_counter") == std::vector<hsize_t>{20});
    CHECK(Dims(file.id, "/frames/device_time_unix_us") == std::vector<hsize_t>{20});
    CHECK(Dims(file.id, "/frames/device_name") == std::vector<hsize_t>{20});
    CHECK(Dims(file.id, "/channels/dist") == std::vector<hsize_t>{20, 841});
    CHECK(Dims(file.id, "/channels/qlty") == std::vector<hsize_t>{20, 841});

    const auto scan_counter = ReadAll<std::uint16_t>(file.id, "/frames/scan_counter", H5T_NATIVE_UINT16);
    const auto telegram_counter = ReadAll<std::uint16_t>(file.id, "/frames/telegram_counter", H5T_NATIVE_UINT16);
    const auto device_time = ReadAll<std::int64_t>(file.id, "/frames/device_time_unix_us", H5T_NATIVE_INT64);
    const auto start_angle = ReadAll<std::int32_t>(file.id, "/frames/start_angle", H5T_NATIVE_INT32);
    for (std::size_t i = 0; i < 20; ++i) {
        CHECK(scan_counter[i] == i);
        CHECK(telegram_counter[i] == i + 1);
        CHECK(device_time[i] == 1700000000000000LL + static_cast<std::int64_t>(i));
        CHECK(start_angle[i] == 550000);
    }

    const auto dist = ReadAll<std::uint16_t>(file.id, "/channels/dist", H5T_NATIVE_UINT16);
    const auto rssi = ReadAll<std::uint16_t>(file.id, "/channels/rssi", H5T_NATIVE_UINT16);
    const auto angl = ReadAll<std::uint16_t>(file.id, "/channels/angl", H5T_NATIVE_UINT16);
    const auto qlty = ReadAll<std::uint8_t>(file.id, "/channels/qlty", H5T_NATIVE_UINT8);
    REQUIRE(dist.size() == 20 * 841);
    for (std::size_t s = 0; s < 20; ++s) {
        for (std::size_t p = 0; p < 841; p += 97) {
            CHECK(dist[s * 841 + p] == ((s * 7 + p) & 0xFFFF));
            CHECK(rssi[s * 841 + p] == p);
            CHECK(angl[s * 841 + p] == 32768 + (p % 5));
            CHECK(qlty[s * 841 + p] == 0x10);
        }
    }

    const auto names = ReadFixedStrings(file.id, "/frames/device_name");
    REQUIRE(names.size() == 20);
    CHECK(names[0] == "unit-test");
    CHECK(names[19] == "unit-test");

    {
        Id dset{H5Dopen2(file.id, "/channels/dist", H5P_DEFAULT), H5Dclose};
        REQUIRE(dset.id >= 0);
        CHECK(ReadStringAttr(dset.id, "content") == "DIST1");
        CHECK(ReadStringAttr(dset.id, "unit") == "mm");
        CHECK(ReadScalarAttr<float>(dset.id, "scale_factor", H5T_NATIVE_FLOAT) == doctest::Approx(0.1f));
        CHECK(ReadScalarAttr<float>(dset.id, "scale_offset", H5T_NATIVE_FLOAT) == doctest::Approx(0.0f));
        CHECK(ReadScalarAttr<std::uint16_t>(dset.id, "channel_mask_bit", H5T_NATIVE_UINT16) ==
            lms4xxx::ChannelMask::kDist1);
    }
    {
        Id dset{H5Dopen2(file.id, "/channels/angl", H5P_DEFAULT), H5Dclose};
        REQUIRE(dset.id >= 0);
        CHECK(ReadScalarAttr<float>(dset.id, "scale_offset", H5T_NATIVE_FLOAT) == doctest::Approx(-32768.0f));
    }
    {
        Id dset{H5Dopen2(file.id, "/frames/angle_step", H5P_DEFAULT), H5Dclose};
        REQUIRE(dset.id >= 0);
        CHECK(ReadStringAttr(dset.id, "unit") == "1e-4 deg");
    }

    fs::remove_all(dir);
}


TEST_CASE("ScanH5File records only the channels in the mask") {
    const auto dir = MakeTempDir("mask");
    const auto path = dir / "scan_unit_20260101_000000_000.h5";
    auto opts = BaseOptions(path);
    opts.channel_mask = lms4xxx::ChannelMask::kDist1 | lms4xxx::ChannelMask::kQlty1;

    {
        lms4xxx::ScanH5File file;
        REQUIRE_MESSAGE(file.Open(opts), file.LastError());
        const auto record = MakeRecord(3);
        REQUIRE_MESSAGE(file.Append(&record, 1), file.LastError());
        file.Close();
    }

    Id file{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    REQUIRE(file.id >= 0);
    CHECK(H5Lexists(file.id, "/channels/dist", H5P_DEFAULT) > 0);
    CHECK(H5Lexists(file.id, "/channels/qlty", H5P_DEFAULT) > 0);
    CHECK(H5Lexists(file.id, "/channels/rssi", H5P_DEFAULT) == 0);
    CHECK(H5Lexists(file.id, "/channels/refl", H5P_DEFAULT) == 0);
    CHECK(H5Lexists(file.id, "/channels/angl", H5P_DEFAULT) == 0);
    CHECK(ReadStringAttr(file.id, "channels") == "dist,qlty");
    CHECK(Dims(file.id, "/channels/dist") == std::vector<hsize_t>{1, 841});

    fs::remove_all(dir);
}


TEST_CASE("ScanH5File gzip round trip") {
    const auto dir = MakeTempDir("gzip");
    const auto path = dir / "scan_unit_20260101_000000_000.h5";
    auto opts = BaseOptions(path);
    opts.compression_level = 4;

    std::vector<lms4xxx::ScanRecord> records;
    for (std::uint32_t i = 0; i < 30; ++i) {
        records.push_back(MakeRecord(i));
    }

    bool compressed = false;
    {
        lms4xxx::ScanH5File file;
        REQUIRE_MESSAGE(file.Open(opts), file.LastError());
        compressed = file.CompressionActive(); // false only when libhdf5 was built without zlib
        REQUIRE_MESSAGE(file.Append(records.data(), 30), file.LastError());
        file.Close();
    }

    Id file{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    REQUIRE(file.id >= 0);
    CHECK(ReadStringAttr(file.id, "compression") == (compressed ? "gzip-4" : "none"));
    const auto dist = ReadAll<std::uint16_t>(file.id, "/channels/dist", H5T_NATIVE_UINT16);
    REQUIRE(dist.size() == 30 * 841);
    for (std::size_t s = 0; s < 30; ++s) {
        for (std::size_t p = 0; p < 841; ++p) {
            if (dist[s * 841 + p] != ((s * 7 + p) & 0xFFFF)) {
                FAIL("dist mismatch at scan " << s << " point " << p);
            }
        }
    }

    fs::remove_all(dir);
}


TEST_CASE("ScanH5File rejects SWMR before creating the incompatible v3 layout") {
    const auto dir = MakeTempDir("swmr");
    const auto path = dir / "scan_unit_20260101_000000_000.h5";
    auto opts = BaseOptions(path);
    opts.swmr = true;

    lms4xxx::ScanH5File file;
    CHECK_FALSE(file.Open(opts));
    CHECK_FALSE(file.IsOpen());
    CHECK(file.LastError().find("SWMR") != std::string::npos);
    CHECK_FALSE(fs::exists(path));

    fs::remove_all(dir);
}


TEST_CASE("ScanH5File fails cleanly on an unwritable path") {
    const auto dir = MakeTempDir("unwritable");
    // A file as "parent directory" fails even as root
    const auto blocker = dir / "blocker";
    {
        std::ofstream(blocker.string()) << "x";
    }
    auto opts = BaseOptions(blocker / "scan.h5");

    lms4xxx::ScanH5File file;
    CHECK_FALSE(file.Open(opts));
    CHECK_FALSE(file.IsOpen());
    CHECK_FALSE(file.LastError().empty());

    lms4xxx::ScanRecord record{};
    CHECK_FALSE(file.Append(&record, 1));
    CHECK_FALSE(file.Flush());

    fs::remove_all(dir);
}


TEST_CASE("ScanH5File rejects invalid options") {
    const auto dir = MakeTempDir("options");
    auto opts = BaseOptions(dir / "scan.h5");

    lms4xxx::ScanH5File file;
    opts.chunk_frames = 0;
    CHECK_FALSE(file.Open(opts));
    opts.chunk_frames = 8;
    opts.compression_level = 10;
    CHECK_FALSE(file.Open(opts));
    CHECK_FALSE(file.IsOpen());

    fs::remove_all(dir);
}


TEST_CASE("Every /frames column round trips, not just the five the golden test reads") {
    // 28 of the 33 columns had no round-trip coverage at all, so a wrong offset
    // in the column table would have shipped silently.
    const auto dir = MakeTempDir("all_columns");
    const auto path = dir / "scan_000.h5";

    lms4xxx::ScanH5File file;
    REQUIRE(file.Open(BaseOptions(path)));

    lms4xxx::ScanRecord r = MakeRecord(3);
    r.meta.transmission_time_us = 0x11223344;
    r.meta.measurement_frequency = 0x21C0;
    r.meta.device_version = 1;
    r.meta.device_number = 0xBEEF;
    r.meta.serial_number = 1605397;
    r.meta.device_status_1 = 1;
    r.meta.device_status_2 = 0;
    r.meta.digital_input_1 = 1;
    r.meta.digital_input_2 = 2;
    r.meta.digital_output_1 = 3;
    r.meta.digital_output_2 = 4;
    r.meta.has_encoder = 1;
    r.meta.encoder_position = 0xDEADBEEF;
    r.meta.has_timestamp = 1;
    r.meta.ts_year = 2026;
    r.meta.ts_month = 9;
    r.meta.ts_day = 4;
    r.meta.ts_hour = 12;
    r.meta.ts_minute = 34;
    r.meta.ts_second = 56;
    r.meta.ts_microsecond = 789012;
    r.meta.y_rotation = 1.25f;
    REQUIRE(file.Append(&r, 1));
    REQUIRE(file.Close());

    Id h5{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    REQUIRE(h5.id >= 0);

    CHECK(ReadAll<std::uint32_t>(h5.id, "/frames/transmission_time_us", H5T_NATIVE_UINT32)[0] == 0x11223344u);
    CHECK(ReadAll<std::uint32_t>(h5.id, "/frames/measurement_frequency", H5T_NATIVE_UINT32)[0] == 0x21C0u);
    CHECK(ReadAll<std::uint16_t>(h5.id, "/frames/device_version", H5T_NATIVE_UINT16)[0] == 1);
    CHECK(ReadAll<std::uint16_t>(h5.id, "/frames/device_number", H5T_NATIVE_UINT16)[0] == 0xBEEF);
    CHECK(ReadAll<std::uint32_t>(h5.id, "/frames/serial_number", H5T_NATIVE_UINT32)[0] == 1605397u);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/device_status_1", H5T_NATIVE_UINT8)[0] == 1);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/device_status_2", H5T_NATIVE_UINT8)[0] == 0);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/digital_input_1", H5T_NATIVE_UINT8)[0] == 1);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/digital_input_2", H5T_NATIVE_UINT8)[0] == 2);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/digital_output_1", H5T_NATIVE_UINT8)[0] == 3);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/digital_output_2", H5T_NATIVE_UINT8)[0] == 4);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/has_encoder", H5T_NATIVE_UINT8)[0] == 1);
    CHECK(ReadAll<std::uint32_t>(h5.id, "/frames/encoder_position", H5T_NATIVE_UINT32)[0] == 0xDEADBEEFu);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/has_timestamp", H5T_NATIVE_UINT8)[0] == 1);
    CHECK(ReadAll<std::uint16_t>(h5.id, "/frames/ts_year", H5T_NATIVE_UINT16)[0] == 2026);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/ts_month", H5T_NATIVE_UINT8)[0] == 9);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/ts_day", H5T_NATIVE_UINT8)[0] == 4);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/ts_hour", H5T_NATIVE_UINT8)[0] == 12);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/ts_minute", H5T_NATIVE_UINT8)[0] == 34);
    CHECK(ReadAll<std::uint8_t>(h5.id, "/frames/ts_second", H5T_NATIVE_UINT8)[0] == 56);
    CHECK(ReadAll<std::uint32_t>(h5.id, "/frames/ts_microsecond", H5T_NATIVE_UINT32)[0] == 789012u);
    CHECK(ReadAll<float>(h5.id, "/frames/y_rotation", H5T_NATIVE_FLOAT)[0] == doctest::Approx(1.25f));

    // The phantom encoder_speed column is gone: the telegram field after the
    // position is Reserved (manual p.93) and never was a speed.
    CHECK(H5Lexists(h5.id, "/frames/encoder_speed", H5P_DEFAULT) == 0);

    fs::remove_all(dir);
}


TEST_CASE("The group attributes that describe the layout are present") {
    const auto dir = MakeTempDir("group_attrs");
    const auto path = dir / "scan_000.h5";
    lms4xxx::ScanH5File file;
    REQUIRE(file.Open(BaseOptions(path)));
    REQUIRE(file.Close());

    Id h5{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    REQUIRE(h5.id >= 0);
    Id frames{H5Gopen2(h5.id, "frames", H5P_DEFAULT), H5Gclose};
    Id channels{H5Gopen2(h5.id, "channels", H5P_DEFAULT), H5Gclose};
    Id telemetry{H5Gopen2(h5.id, "telemetry", H5P_DEFAULT), H5Gclose};
    REQUIRE(frames.id >= 0);
    REQUIRE(channels.id >= 0);
    REQUIRE(telemetry.id >= 0);
    CHECK(H5Aexists(frames.id, "description") > 0);
    CHECK(H5Aexists(channels.id, "description") > 0);
    // The angle formula is how a reader turns a column index into an angle; it
    // is the one piece of maths the file must carry itself.
    CHECK(H5Aexists(channels.id, "angle_formula") > 0);
    CHECK(H5Aexists(channels.id, "points_per_scan") > 0);
    CHECK(H5Aexists(telemetry.id, "description") > 0);

    fs::remove_all(dir);
}


TEST_CASE("An unfinished file has no completeness marker") {
    // The whole point: a recording that was killed must be distinguishable from
    // one that finished. Simulated by never calling Close() on the writer's
    // behalf — the destructor writes the marker, so the check is done while the
    // file is still open through a second, read-only handle.
    const auto dir = MakeTempDir("unfinished");
    const auto path = dir / "scan_000.h5";
    {
        lms4xxx::ScanH5File file;
        auto opts = BaseOptions(path);
        // A second handle in this same process inspects the open file. This
        // does not claim that an external non-SWMR reader can open a live file.
        REQUIRE(file.Open(opts));
        const auto record = MakeRecord(0);
        REQUIRE(file.Append(&record, 1));
        REQUIRE(file.Flush());

        Id h5{
            H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT),
            H5Fclose
        };
        REQUIRE(h5.id >= 0);
        CHECK(H5Aexists(h5.id, "closed_cleanly") == 0);
    }
    fs::remove_all(dir);
}


TEST_CASE("Opening a path that already exists fails instead of truncating") {
    const auto dir = MakeTempDir("excl");
    const auto path = dir / "scan_000.h5";
    { std::ofstream(path.string()) << "existing data"; }

    lms4xxx::ScanH5File file;
    CHECK_FALSE(file.Open(BaseOptions(path)));
    CHECK_FALSE(file.IsOpen());
    CHECK(!file.LastError().empty());
    // The existing bytes are untouched.
    CHECK(fs::file_size(path) == 13);

    fs::remove_all(dir);
}

TEST_CASE("A failed append cannot regain a clean marker by closing") {
    const auto dir = MakeTempDir("failed_append_marker");
    const auto path = dir / "scan.h5";
    lms4xxx::ScanH5File file;
    REQUIRE(file.Open(BaseOptions(path)));
    CHECK_FALSE(file.Append(nullptr, 1));
    CHECK_FALSE(file.Close());
    CHECK_FALSE(file.Close());
    {
        Id h5{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
        REQUIRE(h5.id >= 0);
        Id attr{H5Aopen(h5.id, "closed_cleanly", H5P_DEFAULT), H5Aclose};
        REQUIRE(attr.id >= 0);
        std::uint8_t clean = 1;
        REQUIRE(H5Aread(attr.id, H5T_NATIVE_UINT8, &clean) >= 0);
        CHECK(clean == 0);
    }
    fs::remove_all(dir);
}


TEST_CASE("The device audit becomes root attributes, and only for fields that were read") {
    const auto dir = MakeTempDir("audit");
    const auto path = dir / "scan_000.h5";
    auto opts = BaseOptions(path);
    opts.audit.temperature_c = 41.5;
    opts.audit.scan_frequency_centi_hz = 60000; // 600 Hz (manual p.73)
    opts.audit.angular_resolution_1e4 = 833; // 1/12 deg
    opts.audit.laser_trigger_source = 0; // free-running, as continuous output needs
    opts.audit.laser_timeout_s = 0;
    // motor_sync_* deliberately left unread here.

    lms4xxx::ScanH5File file;
    REQUIRE(file.Open(opts));
    REQUIRE(file.Close());

    Id h5{H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    REQUIRE(h5.id >= 0);

    const auto f64 = [&](const char *name) {
        Id attr{H5Aopen(h5.id, name, H5P_DEFAULT), H5Aclose};
        REQUIRE(attr.id >= 0);
        double value = 0.0;
        REQUIRE(H5Aread(attr.id, H5T_NATIVE_DOUBLE, &value) >= 0);
        return value;
    };
    CHECK(f64("device_temperature_c_at_start") == doctest::Approx(41.5));
    CHECK(f64("device_scan_frequency_hz") == doctest::Approx(600.0));
    CHECK(f64("device_angular_resolution_deg") == doctest::Approx(0.0833));
    CHECK(H5Aexists(h5.id, "device_laser_trigger_source") > 0);
    CHECK(H5Aexists(h5.id, "device_motor_sync_role") == 0); // never read: no attribute

    // Version 4 retains the v3 telemetry/close markers and adds clock quality.
    Id version{H5Aopen(h5.id, "format_version", H5P_DEFAULT), H5Aclose};
    REQUIRE(version.id >= 0);
    std::uint32_t value = 0;
    REQUIRE(H5Aread(version.id, H5T_NATIVE_UINT32, &value) >= 0);
    CHECK(value == 4);

    fs::remove_all(dir);
}
