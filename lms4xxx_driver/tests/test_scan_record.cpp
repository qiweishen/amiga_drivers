/// @file test_scan_record.cpp
/// @brief ScanData -> ScanRecord conversion, timestamps and payload sizing.
///
/// This layer decides what actually reaches the .h5 file, and none of it was
/// tested: the timestamp rejection paths, the geometry fallbacks, the "channels
/// outside the mask are left untouched" contract and the byte accounting that
/// drives both the bytes= statistic and the file-split threshold.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "scan_record.h"


using lms4xxx::ChannelContent16;
using lms4xxx::ChannelContent8;
namespace ChannelMask = lms4xxx::ChannelMask;
using lms4xxx::ScanData;
using lms4xxx::ScanRecord;
using lms4xxx::ScanTimestamp;

namespace {
    lms4xxx::ChannelData16 Make16(ChannelContent16 content, std::uint16_t num_data, std::uint16_t first = 0) {
        lms4xxx::ChannelData16 ch;
        ch.content = content;
        ch.num_data = num_data;
        ch.start_angle = 550000;
        ch.angle_step = 833;
        ch.data.resize(num_data);
        for (std::uint16_t i = 0; i < num_data; ++i) {
            ch.data[i] = static_cast<std::uint16_t>(first + i);
        }
        return ch;
    }

    lms4xxx::ChannelData8 Make8(std::uint16_t num_data, std::uint8_t first = 0x10) {
        lms4xxx::ChannelData8 ch;
        ch.content = ChannelContent8::kQlty1;
        ch.num_data = num_data;
        ch.start_angle = 550000;
        ch.angle_step = 833;
        ch.data.resize(num_data);
        for (std::uint16_t i = 0; i < num_data; ++i) {
            ch.data[i] = static_cast<std::uint8_t>(first + i);
        }
        return ch;
    }

    ScanData FullScan(std::uint16_t points = 8) {
        ScanData scan;
        scan.telegram_counter = 42;
        scan.scan_counter = 41;
        scan.scan_frequency = 60000;
        scan.channels_16bit = {
            Make16(ChannelContent16::kDist1, points, 1000),
            Make16(ChannelContent16::kRssi1, points, 200),
            Make16(ChannelContent16::kAngl1, points, 32768),
        };
        scan.channels_8bit = {Make8(points)};
        return scan;
    }

    constexpr std::uint16_t kDefaultMask =
            ChannelMask::kDist1 | ChannelMask::kRssi1 | ChannelMask::kAngl1 | ChannelMask::kQlty1;
} // namespace


TEST_CASE("FillScanRecord copies the masked channels and zero-pads the rest of the row") {
    const auto scan = FullScan(8);
    ScanRecord record{};
    std::memset(&record, 0xAB, sizeof(record)); // poison: nothing may be left over
    lms4xxx::FillScanRecord(scan, kDefaultMask, record);

    CHECK(record.meta.telegram_counter == 42);
    CHECK(record.meta.scan_counter == 41);
    CHECK(record.meta.num_points == 8);
    CHECK(record.meta.start_angle == 550000);
    CHECK(record.meta.angle_step == 833);

    CHECK(record.dist[0] == 1000);
    CHECK(record.dist[7] == 1007);
    CHECK(record.rssi[0] == 200);
    CHECK(record.angl[0] == 32768);
    CHECK(record.qlty[0] == 0x10);

    // Everything past num_points is zeroed, which is the invariant every reader
    // (and scripts/inspect_h5.py) relies on.
    CHECK(record.dist[8] == 0);
    CHECK(record.dist[lms4xxx::kMaxPointsPerScan - 1] == 0);
    CHECK(record.qlty[8] == 0);
}

TEST_CASE("Channels outside the mask are left untouched") {
    // The writer only stores the masked datasets, so FillScanRecord must not
    // spend time on the others — but it also must not leave them half-filled.
    const auto scan = FullScan(4);
    ScanRecord record{};
    for (std::size_t i = 0; i < lms4xxx::kMaxPointsPerScan; ++i) {
        record.refl[i] = 0x5A5A;
    }
    lms4xxx::FillScanRecord(scan, kDefaultMask, record); // no kRefl1 in the mask
    CHECK(record.refl[0] == 0x5A5A);
    CHECK(record.dist[0] == 1000);
}

TEST_CASE("A channel shorter than it claims is truncated, never over-read") {
    ScanData scan = FullScan(4);
    scan.channels_16bit[0].num_data = 600; // lies about the point count
    ScanRecord record{};
    lms4xxx::FillScanRecord(scan, kDefaultMask, record);
    CHECK(record.dist[0] == 1000);
    CHECK(record.dist[3] == 1003);
    CHECK(record.dist[4] == 0); // beyond the data that actually exists
}

TEST_CASE("num_points is clamped to the aperture") {
    ScanData scan = FullScan(4);
    scan.channels_16bit[0].num_data = 5000;
    ScanRecord record{};
    lms4xxx::FillScanRecord(scan, kDefaultMask, record);
    CHECK(record.meta.num_points == lms4xxx::kMaxPointsPerScan);
}

TEST_CASE("Geometry falls back when the distance channel is absent") {
    SUBCASE("to the first 16-bit channel") {
        ScanData scan;
        scan.channels_16bit = {Make16(ChannelContent16::kRssi1, 5, 7)};
        scan.channels_16bit[0].start_angle = 600000;
        ScanRecord record{};
        lms4xxx::FillScanRecord(scan, kDefaultMask, record);
        CHECK(record.meta.num_points == 5);
        CHECK(record.meta.start_angle == 600000);
    }
    SUBCASE("to the 8-bit channel when there is no 16-bit one at all") {
        ScanData scan;
        scan.channels_8bit = {Make8(3)};
        ScanRecord record{};
        lms4xxx::FillScanRecord(scan, kDefaultMask, record);
        CHECK(record.meta.num_points == 3);
    }
    SUBCASE("to nothing at all, which stays zero rather than guessing") {
        const ScanData scan;
        ScanRecord record{};
        lms4xxx::FillScanRecord(scan, kDefaultMask, record);
        CHECK(record.meta.num_points == 0);
        CHECK(record.meta.start_angle == 0);
    }
}

TEST_CASE("The device name is copied and always NUL-terminated") {
    ScanData scan = FullScan(2);
    scan.has_device_name = true;
    scan.device_name = "0123456789ABCDEF"; // 16 chars into a 16-byte field
    ScanRecord record{};
    lms4xxx::FillScanRecord(scan, kDefaultMask, record);
    CHECK(record.meta.has_device_name == 1);
    CHECK(std::string(record.meta.device_name) == "0123456789ABCDE"); // 15 + NUL
    CHECK(record.meta.device_name[15] == '\0');
}

TEST_CASE("DeviceTimeUnixUs converts a valid UTC telegram timestamp") {
    ScanTimestamp ts;
    ts.year = 1970;
    ts.month = 1;
    ts.day = 1;
    CHECK(lms4xxx::DeviceTimeUnixUs(ts) == 0);

    ts.year = 2026;
    ts.month = 9;
    ts.day = 4;
    ts.hour = 12;
    ts.minute = 34;
    ts.second = 56;
    ts.microsecond = 789012;
    // date -u -d '2026-09-04 12:34:56' +%s = 1788525296
    CHECK(lms4xxx::DeviceTimeUnixUs(ts) == 1788525296LL * 1000000 + 789012);
}

TEST_CASE("DeviceTimeUnixUs handles leap days and pre-epoch dates") {
    ScanTimestamp ts;
    ts.year = 2024; // a leap year
    ts.month = 2;
    ts.day = 29;
    CHECK(lms4xxx::DeviceTimeUnixUs(ts) == 1709164800LL * 1000000);

    ts.year = 2000; // the century leap year
    ts.month = 3;
    ts.day = 1;
    CHECK(lms4xxx::DeviceTimeUnixUs(ts) == 951868800LL * 1000000);

    ts.year = 1969; // before the epoch: must go negative, not wrap
    ts.month = 12;
    ts.day = 31;
    ts.hour = 23;
    ts.minute = 59;
    ts.second = 59;
    ts.microsecond = 0;
    CHECK(lms4xxx::DeviceTimeUnixUs(ts) == -1000000LL);
}

TEST_CASE("DeviceTimeUnixUs rejects every out-of-range field with 0") {
    // The device has no real-time clock (manual p.97); until NTP or
    // LSPsetdatetime sets it, the timestamp block can contain anything. 0 is the
    // documented "no usable device time" value.
    const auto rejected = [](const ScanTimestamp &ts) { return lms4xxx::DeviceTimeUnixUs(ts) == 0; };
    ScanTimestamp base;
    base.year = 2026;
    base.month = 9;
    base.day = 4;

    ScanTimestamp t = base;
    t.month = 0;
    CHECK(rejected(t));
    t = base;
    t.month = 13;
    CHECK(rejected(t));
    t = base;
    t.day = 0;
    CHECK(rejected(t));
    t = base;
    t.day = 32;
    CHECK(rejected(t));
    t = base;
    t.hour = 24;
    CHECK(rejected(t));
    t = base;
    t.minute = 60;
    CHECK(rejected(t));
    t = base;
    t.second = 61;
    CHECK(rejected(t));
    t = base;
    t.microsecond = 1000000;
    CHECK(rejected(t));

    // A leap second (second == 60) is accepted, not rejected.
    t = base;
    t.second = 60;
    CHECK(lms4xxx::DeviceTimeUnixUs(t) != 0);
}

TEST_CASE("RecordBytes counts what actually lands on disk") {
    // sizeof(FrameMeta) carries alignment padding that never reaches the file;
    // using it made bytes= and the max_file_bytes split threshold overstate the
    // payload by 14 bytes per scan.
    CHECK(lms4xxx::FrameMetaBytesOnDisk() < sizeof(lms4xxx::FrameMeta));

    const std::uint32_t meta = lms4xxx::FrameMetaBytesOnDisk();
    CHECK(lms4xxx::RecordBytes(0) == meta);
    CHECK(lms4xxx::RecordBytes(ChannelMask::kQlty1) == meta + lms4xxx::kMaxPointsPerScan * 1);
    CHECK(lms4xxx::RecordBytes(ChannelMask::kDist1) == meta + lms4xxx::kMaxPointsPerScan * 2);
    // The shipped mask: three 16-bit channels and one 8-bit channel.
    CHECK(lms4xxx::RecordBytes(kDefaultMask) == meta + lms4xxx::kMaxPointsPerScan * (2 + 2 + 2 + 1));
}

TEST_CASE("ChannelNames lists the recorded channels in dataset order") {
    CHECK(lms4xxx::ChannelNames(kDefaultMask) == "dist,rssi,angl,qlty");
    CHECK(lms4xxx::ChannelNames(ChannelMask::kDist1) == "dist");
    CHECK(lms4xxx::ChannelNames(0).empty());
}
