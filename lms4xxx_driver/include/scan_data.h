#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>


namespace lms4xxx {
    inline constexpr std::size_t kMaxPointsPerScan = 841;




    enum class DeviceStatus : std::uint8_t {
        kOk = 0x00,
        kError = 0x01,
    };


    enum class ChannelContent16 : std::uint8_t {
        kDist1 = 0, ///< Distance (1/10 mm, scale 0.1)
        kRssi1 = 1, ///< Signal strength (digit, scale 1.0)
        kRefl1 = 2, ///< Calibrated reflectance (%, scale 0.01)
        kAngl1 = 3, ///< Angle correction (1/10000 deg, scale 1.0, offset -32768)
        kUnknown = 0xFF,
    };


    enum class ChannelContent8 : std::uint8_t {
        kQlty1 = 0, ///< Quality bitfield
        kUnknown = 0xFF,
    };


    // QLTY1 bits
    namespace Quality {
        inline constexpr std::uint8_t kBelowSignalLower = 0x01;
        inline constexpr std::uint8_t kAboveSignalUpper = 0x02;
        inline constexpr std::uint8_t kBelowDistLower = 0x04;
        inline constexpr std::uint8_t kAboveDistUpper = 0x08;
        inline constexpr std::uint8_t kNormalMeasurement = 0x10;
        inline constexpr std::uint8_t kEdgeHitPossible = 0x20;
        inline constexpr std::uint8_t kEdgeHitLikely = 0x30;
        inline constexpr std::uint8_t kSuspectedOutlier = 0x40;
        inline constexpr std::uint8_t kPartialGloss = 0x80;
    } // namespace Quality


    // DIST1 raw values that are not measurements
    namespace InvalidDistance {
        inline constexpr std::uint16_t kInvalidDark = 0; ///< Too dark / out of range / filtered
        inline constexpr std::uint16_t kInvalidBright = 1; ///< Reflection too strong
        inline constexpr std::uint16_t kReservedMax = 15;
    } // namespace InvalidDistance


    // physical = data[i] * scale_factor + scale_offset
    struct ChannelData16 {
        ChannelContent16 content = ChannelContent16::kUnknown;
        float scale_factor = 1.0f; ///< IEEE754 scaling factor
        float scale_offset = 0.0f; ///< IEEE754 scaling offset
        std::int32_t start_angle = 0; ///< 1/10000 degrees
        std::uint16_t angle_step = 0; ///< 1/10000 degrees
        std::uint16_t num_data = 0; ///< Number of measurement points
        std::vector<std::uint16_t> data;

        float ScaledValue(std::size_t i) const {
            return static_cast<float>(data.at(i)) * scale_factor + scale_offset;
        }
    };


    struct ChannelData8 {
        ChannelContent8 content = ChannelContent8::kUnknown;
        float scale_factor = 1.0f;
        float scale_offset = 0.0f;
        std::int32_t start_angle = 0; ///< 1/10000 degrees
        std::uint16_t angle_step = 0; ///< 1/10000 degrees
        std::uint16_t num_data = 0;
        std::vector<std::uint8_t> data;

        float ScaledValue(std::size_t i) const {
            return static_cast<float>(data.at(i)) * scale_factor + scale_offset;
        }
    };


    // Position only; the field after it is Reserved (manual p.93)
    struct EncoderData {
        std::uint32_t position = 0;
    };


    struct ScanTimestamp {
        std::uint16_t year = 0;
        std::uint8_t month = 0;
        std::uint8_t day = 0;
        std::uint8_t hour = 0;
        std::uint8_t minute = 0;
        std::uint8_t second = 0;
        std::uint32_t microsecond = 0;
    };


    struct ScanDeviceInfo {
        std::uint16_t version_number = 0;
        std::uint16_t device_number = 0;
        std::uint32_t serial_number = 0; ///< YYWW format
        DeviceStatus device_status_1 = DeviceStatus::kOk;
        DeviceStatus device_status_2 = DeviceStatus::kOk;
    };


    // One sSN/sRA LMDscandata telegram
    struct ScanData {
        // --- Device Info ---
        ScanDeviceInfo device_info;

        // --- Counters ---
        std::uint16_t telegram_counter = 0; ///< Increments per telegram
        std::uint16_t scan_counter = 0; ///< Increments per scan

        // --- Timing ---
        std::uint32_t time_since_startup_us = 0; ///< Microseconds since device boot
        std::uint32_t transmission_time_us = 0; ///< Microseconds transmission duration

        // --- Digital I/O ---
        std::uint8_t digital_input_1 = 0;
        std::uint8_t digital_input_2 = 0;
        std::uint8_t digital_output_1 = 0;
        std::uint8_t digital_output_2 = 0;

        // --- Frequency ---
        std::uint32_t scan_frequency = 0; ///< 1/100 Hz (600 Hz = 0x0000EA60)
        std::uint32_t measurement_frequency = 0; ///< Hz (typically 100)

        // --- Encoder (optional) ---
        bool has_encoder = false;
        EncoderData encoder;

        // --- 16-bit channels ---
        std::vector<ChannelData16> channels_16bit;

        // --- 8-bit channels ---
        std::vector<ChannelData8> channels_8bit;

        // --- Position (reserved) ---
        float y_rotation = 0.0f;

        // --- Device name (optional) ---
        bool has_device_name = false;
        std::string device_name;

        // --- Timestamp (optional) ---
        bool has_timestamp = false;
        ScanTimestamp timestamp;

        // Channel lookups: nullptr when absent
        const ChannelData16 *DistanceChannel() const {
            for (const auto &ch: channels_16bit) {
                if (ch.content == ChannelContent16::kDist1)
                    return &ch;
            }
            return nullptr;
        }

        const ChannelData16 *RssiChannel() const {
            for (const auto &ch: channels_16bit) {
                if (ch.content == ChannelContent16::kRssi1)
                    return &ch;
            }
            return nullptr;
        }

        const ChannelData16 *ReflectanceChannel() const {
            for (const auto &ch: channels_16bit) {
                if (ch.content == ChannelContent16::kRefl1)
                    return &ch;
            }
            return nullptr;
        }

        const ChannelData16 *AngleCorrectionChannel() const {
            for (const auto &ch: channels_16bit) {
                if (ch.content == ChannelContent16::kAngl1)
                    return &ch;
            }
            return nullptr;
        }

        const ChannelData8 *QualityChannel() const {
            for (const auto &ch: channels_8bit) {
                if (ch.content == ChannelContent8::kQlty1)
                    return &ch;
            }
            return nullptr;
        }

        // Points of the distance channel
        std::uint16_t NumPoints() const {
            const auto *dist = DistanceChannel();
            return dist ? dist->num_data : 0;
        }

        float ScanFrequencyHz() const { return static_cast<float>(scan_frequency) / 100.0f; }
    };
} // namespace lms4xxx

