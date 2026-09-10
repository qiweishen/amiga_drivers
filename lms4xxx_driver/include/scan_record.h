#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

#include "scan_data.h"


namespace lms4xxx {
    namespace ChannelMask {
        inline constexpr std::uint16_t kDist1 = 0x01;
        inline constexpr std::uint16_t kRssi1 = 0x02;
        inline constexpr std::uint16_t kRefl1 = 0x04;
        inline constexpr std::uint16_t kAngl1 = 0x08;
        inline constexpr std::uint16_t kQlty1 = 0x10;
        inline constexpr std::uint16_t kAll = 0x1F;
    } // namespace ChannelMask


    struct FrameMeta {
        // --- Device timing ---
        std::int64_t device_time_unix_us = 0; ///< Telegram timestamp as Unix us; 0 when absent
        std::uint32_t time_since_startup_us = 0;
        std::uint32_t transmission_time_us = 0;

        // --- Counters / scan geometry ---
        std::uint16_t telegram_counter = 0;
        std::uint16_t scan_counter = 0;
        std::uint16_t num_points = 0;
        std::int32_t start_angle = 0; ///< 1/10000 deg
        std::uint16_t angle_step = 0; ///< 1/10000 deg
        std::uint32_t scan_frequency = 0; ///< 1/100 Hz
        std::uint32_t measurement_frequency = 0; ///< 100 Hz units

        // --- Device info ---
        std::uint16_t device_version = 0;
        std::uint16_t device_number = 0;
        std::uint32_t serial_number = 0;
        std::uint8_t device_status_1 = 0;
        std::uint8_t device_status_2 = 0;

        // --- Digital I/O ---
        std::uint8_t digital_input_1 = 0;
        std::uint8_t digital_input_2 = 0;
        std::uint8_t digital_output_1 = 0;
        std::uint8_t digital_output_2 = 0;

        // --- Encoder (optional) ---
        std::uint8_t has_encoder = 0;
        std::uint32_t encoder_position = 0;

        // --- Timestamp (optional) ---
        std::uint8_t has_timestamp = 0;
        std::uint16_t ts_year = 0;
        std::uint8_t ts_month = 0;
        std::uint8_t ts_day = 0;
        std::uint8_t ts_hour = 0;
        std::uint8_t ts_minute = 0;
        std::uint8_t ts_second = 0;
        std::uint32_t ts_microsecond = 0;

        // --- Position / name ---
        float y_rotation = 0.0f;
        std::uint8_t has_device_name = 0;
        char device_name[16] = {}; ///< NUL-terminated
    };


    // Only the channels in the recording mask are filled; rows zero-padded beyond num_points
    struct ScanRecord {
        FrameMeta meta;
        std::uint16_t dist[kMaxPointsPerScan];
        std::uint16_t rssi[kMaxPointsPerScan];
        std::uint16_t refl[kMaxPointsPerScan];
        std::uint16_t angl[kMaxPointsPerScan];
        std::uint8_t qlty[kMaxPointsPerScan];
    };

    static_assert(std::is_standard_layout_v<ScanRecord>, "ScanRecord must be standard-layout (offsetof)");


    // Documented scaling (physical = raw * scale_factor + scale_offset), written
    // as dataset attributes; every scan is rejected if the device reports otherwise
    struct ChannelSpec {
        const char *name; ///< dataset name under /channels
        std::uint16_t bit; ///< ChannelMask bit
        std::size_t record_offset; ///< offset of the array inside ScanRecord
        std::size_t elem_size; ///< bytes per point (2 or 1)
        const char *content; ///< telegram content id ("DIST1", ...)
        float scale_factor;
        float scale_offset;
        const char *unit;
        const char *description;
    };

    inline constexpr ChannelSpec kChannelSpecs[] = {
        {
            "dist", ChannelMask::kDist1, offsetof(ScanRecord, dist), sizeof(std::uint16_t), "DIST1", 0.1f, 0.0f, "mm",
            "Radial distance per point. distance_mm = raw * scale_factor + scale_offset. Raw values 0..15 are not "
            "measurements: 0 = too dark / out of range / filtered, 1 = reflection too strong, 2..15 reserved."
        },
        {
            "rssi", ChannelMask::kRssi1, offsetof(ScanRecord, rssi), sizeof(std::uint16_t), "RSSI1", 1.0f, 0.0f,
            "digit", "Received signal strength per point (device digits)."
        },
        {
            "refl", ChannelMask::kRefl1, offsetof(ScanRecord, refl), sizeof(std::uint16_t), "REFL1", 0.01f, 0.0f,
            "percent", "Calibrated reflectance per point. reflectance_percent = raw * scale_factor + scale_offset."
        },
        {
            "angl", ChannelMask::kAngl1, offsetof(ScanRecord, angl), sizeof(std::uint16_t), "ANGL1", 1.0f, -32768.0f,
            "1e-4 deg",
            "Per-point angle correction. correction_deg = (raw * scale_factor + scale_offset) / 1e4, "
            "i.e. (raw - 32768) / 1e4."
        },
        {
            "qlty", ChannelMask::kQlty1, offsetof(ScanRecord, qlty), sizeof(std::uint8_t), "QLTY1", 1.0f, 0.0f,
            "bitfield",
            "Quality bitfield per point: 0x01 below signal lower limit, 0x02 above signal upper limit, 0x04 below "
            "distance lower limit, 0x08 above distance upper limit, 0x10 normal measurement, 0x20 edge hit possible, "
            "0x30 edge hit likely, 0x40 suspected outlier, 0x80 partial gloss."
        },
    };

    inline constexpr std::size_t kChannelSpecCount = sizeof(kChannelSpecs) / sizeof(kChannelSpecs[0]);


    // Only the channels in `mask` are touched
    void FillScanRecord(const ScanData &scan, std::uint16_t mask, ScanRecord &out);

    // Packed width of one /frames row on disk (excludes the struct padding)
    std::uint32_t FrameMetaBytesOnDisk();

    // Logical payload of one recorded scan: the /frames row plus 841 points per
    // channel in `mask`. Uncompressed, as stored, which is what bytes= counts
    // and what max_file_bytes is compared against.
    std::uint32_t RecordBytes(std::uint16_t mask);

    // "dist,rssi,angl,qlty"
    std::string ChannelNames(std::uint16_t mask);

    // Telegram timestamp (UTC) -> us since the Unix epoch; 0 for out-of-range fields
    std::int64_t DeviceTimeUnixUs(const ScanTimestamp &ts);
} // namespace lms4xxx
