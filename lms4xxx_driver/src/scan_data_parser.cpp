#include "scan_data_parser.h"

#include <cstring>

#include "cola_b.h"
#include "error.h"
#include "logger.h"
#include "utility.h"


namespace {
    constexpr std::string_view kModule = "LMS4xxx";

    common::DriverLog g_log{std::string(kModule)};

    // DIST1, RSSI1, REFL1, ANGL1
    constexpr std::uint16_t kMax16BitChannelCount = 4;
    constexpr std::uint16_t kMaxEncoderCount = 2; // manual p.93: at most one encoder block, tolerate two

    // QLTY1
    constexpr std::uint16_t kMax8BitChannelCount = 1;
} // namespace


namespace lms4xxx {
    std::uint8_t ScanDataParser::Reader::ReadUint8() {
        return data[pos++];
    }

    std::uint16_t ScanDataParser::Reader::ReadUint16() {
        const auto val = ColaBCodec::DecodeUint16(data + pos);
        pos += 2;
        return val;
    }

    std::int32_t ScanDataParser::Reader::ReadInt32() {
        const auto val = ColaBCodec::DecodeInt32(data + pos);
        pos += 4;
        return val;
    }

    std::uint32_t ScanDataParser::Reader::ReadUint32() {
        const auto val = ColaBCodec::DecodeUint32(data + pos);
        pos += 4;
        return val;
    }

    float ScanDataParser::Reader::ReadFloat() {
        const auto val = ColaBCodec::DecodeFloat(data + pos);
        pos += 4;
        return val;
    }

    std::string ScanDataParser::Reader::ReadString(std::size_t length) {
        auto val = ColaBCodec::DecodeString(data + pos, length);
        pos += length;
        return val;
    }

    void ScanDataParser::Reader::Skip(std::size_t n) {
        pos += n;
    }

    std::error_code ScanDataParser::Parse(const std::uint8_t *data, std::size_t len, ScanData &out,
                                          std::string_view tag) {
        out = ScanData{};

        if (len < 52) {
            g_log.Warn("[{}] Payload too short: {} bytes (minimum ~52)", tag, len);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        Reader r{data, len, 0, tag};

        // Sections in telegram order
        std::error_code ec;

        ec = ParseDeviceInfo(r, out);
        if (ec) {
            return ec;
        }

        ec = ParseFrequency(r, out);
        if (ec) {
            return ec;
        }

        ec = ParseEncoder(r, out);
        if (ec) {
            return ec;
        }

        ec = ParseChannels16bit(r, out);
        if (ec) {
            return ec;
        }

        ec = ParseChannels8bit(r, out);
        if (ec) {
            return ec;
        }

        ec = ParsePosition(r, out);
        if (ec) {
            return ec;
        }

        ec = ParseDeviceName(r, out);
        if (ec) {
            return ec;
        }

        ec = ParseTimestamp(r, out);
        if (ec) {
            return ec;
        }

        // Trailing reserved field (2B), optional
        if (r.HasBytes(2)) {
            r.Skip(2);
        }
        // Everything must be consumed: bytes left over mean the layout is not the one assumed
        if (r.Remaining() != 0) {
            g_log.Warn("[{}] {} unparsed byte(s) at the end of LMDscandata (telegram layout mismatch?)", r.tag,
                       r.Remaining());
            return make_error_code(ErrorCode::kProtocolError);
        }
        return {};
    }

    std::error_code ScanDataParser::ParseDeviceInfo(Reader &r, ScanData &out) {
        // Version(2) DeviceNo(2) Serial(4) Status(2x1) TelegramCtr(2) ScanCtr(2)
        // TimeSinceStartup(4) TransmissionTime(4) DigIn(2x1) DigOut(2x1) Reserved(2)
        if (!r.HasBytes(28)) {
            g_log.Warn("[{}] Truncated device info block, pos={}", r.tag, r.pos);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        out.device_info.version_number = r.ReadUint16();
        if (out.device_info.version_number != 1) {
            g_log.Warn("[{}] Unexpected version number: {} (expected 1)", r.tag, out.device_info.version_number);
        }

        out.device_info.device_number = r.ReadUint16();
        out.device_info.serial_number = r.ReadUint32();
        out.device_info.device_status_1 = static_cast<DeviceStatus>(r.ReadUint8());
        out.device_info.device_status_2 = static_cast<DeviceStatus>(r.ReadUint8());

        out.telegram_counter = r.ReadUint16();
        out.scan_counter = r.ReadUint16();

        out.time_since_startup_us = r.ReadUint32();
        out.transmission_time_us = r.ReadUint32();

        out.digital_input_1 = r.ReadUint8();
        out.digital_input_2 = r.ReadUint8();
        out.digital_output_1 = r.ReadUint8();
        out.digital_output_2 = r.ReadUint8();

        // Reserved
        r.Skip(2);

        return {};
    }

    std::error_code ScanDataParser::ParseFrequency(Reader &r, ScanData &out) {
        // ScanFrequency(4) MeasurementFrequency(4)
        if (!r.HasBytes(8)) {
            g_log.Warn("[{}] Truncated frequency block, pos={}", r.tag, r.pos);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        out.scan_frequency = r.ReadUint32();
        out.measurement_frequency = r.ReadUint32();

        return {};
    }

    std::error_code ScanDataParser::ParseEncoder(Reader &r, ScanData &out) {
        // EncoderCount(2)
        if (!r.HasBytes(2)) {
            g_log.Warn("[{}] Truncated encoder count, pos={}", r.tag, r.pos);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        const auto encoder_count = r.ReadUint16();
        if (encoder_count > kMaxEncoderCount) {
            g_log.Warn("[{}] Encoder count {} out of range, pos={}", r.tag, encoder_count, r.pos);
            return make_error_code(ErrorCode::kProtocolError);
        }
        out.has_encoder = (encoder_count > 0);

        // EVERY block must be consumed, not just the first: leaving a second
        // one in the buffer shifts every later section and the telegram then
        // fails as "too short" somewhere unrelated. Only the first block is
        // reported (the driver configures no encoder at all, manual p.85).
        for (std::uint16_t i = 0; i < encoder_count; ++i) {
            // Per encoder: Position(4) Reserved(2)
            if (!r.HasBytes(6)) {
                g_log.Warn("[{}] Truncated encoder data, pos={}", r.tag, r.pos);
                return make_error_code(ErrorCode::kFrameTooShort);
            }
            const auto position = r.ReadUint32();
            r.Skip(2); // Reserved (manual p.93), NOT a speed
            if (i == 0) {
                out.encoder.position = position;
            }
        }

        return {};
    }

    std::error_code ScanDataParser::ParseChannels16bit(Reader &r, ScanData &out) {
        // ChannelCount(2)
        if (!r.HasBytes(2)) {
            g_log.Warn("[{}] Truncated 16-bit channel count, pos={}", r.tag, r.pos);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        const auto channel_count = r.ReadUint16();

        if (channel_count > kMax16BitChannelCount) {
            g_log.Warn("[{}] Invalid 16-bit channel count: {} (max {})", r.tag, channel_count, kMax16BitChannelCount);
            return make_error_code(ErrorCode::kProtocolError);
        }

        out.channels_16bit.reserve(channel_count);

        for (std::uint16_t ch = 0; ch < channel_count; ch++) {
            // Content(5) Scale(4) Offset(4) StartAngle(4) AngularStep(2) DataCount(2)
            constexpr std::size_t kChannelHeaderSize = 5 + 4 + 4 + 4 + 2 + 2;
            if (!r.HasBytes(kChannelHeaderSize)) {
                g_log.Warn("[{}] Truncated 16-bit channel {} header, pos={}", r.tag, ch, r.pos);
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            ChannelData16 channel;

            const auto name = r.ReadString(5);
            channel.content = IdentifyChannel16(name, r.tag);

            channel.scale_factor = r.ReadFloat();
            channel.scale_offset = r.ReadFloat();
            channel.start_angle = r.ReadInt32();
            channel.angle_step = r.ReadUint16();
            channel.num_data = r.ReadUint16();

            if (channel.num_data > kMaxPointsPerScan) {
                g_log.Warn("[{}] Channel {} data count {} exceeds maximum {}", r.tag, name, channel.num_data,
                           kMaxPointsPerScan);
                return make_error_code(ErrorCode::kProtocolError);
            }

            const std::size_t data_bytes = static_cast<std::size_t>(channel.num_data) * 2;
            if (!r.HasBytes(data_bytes)) {
                g_log.Warn("[{}] Truncated 16-bit channel {} data: need {} bytes, have {}", r.tag, name, data_bytes,
                           r.Remaining());
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            channel.data.resize(channel.num_data);
            for (std::uint16_t i = 0; i < channel.num_data; i++) {
                channel.data[i] = r.ReadUint16();
            }

            out.channels_16bit.push_back(std::move(channel));
        }

        return {};
    }

    std::error_code ScanDataParser::ParseChannels8bit(Reader &r, ScanData &out) {
        // ChannelCount(2)
        if (!r.HasBytes(2)) {
            g_log.Warn("[{}] Truncated 8-bit channel count, pos={}", r.tag, r.pos);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        const auto channel_count = r.ReadUint16();

        if (channel_count > kMax8BitChannelCount) {
            g_log.Warn("[{}] Invalid 8-bit channel count: {} (max {})", r.tag, channel_count, kMax8BitChannelCount);
            return make_error_code(ErrorCode::kProtocolError);
        }

        out.channels_8bit.reserve(channel_count);

        for (std::uint16_t ch = 0; ch < channel_count; ch++) {
            // Same header layout as the 16-bit channels
            constexpr std::size_t kChannelHeaderSize = 5 + 4 + 4 + 4 + 2 + 2;
            if (!r.HasBytes(kChannelHeaderSize)) {
                g_log.Warn("[{}] Truncated 8-bit channel {} header, pos={}", r.tag, ch, r.pos);
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            ChannelData8 channel;

            const auto name = r.ReadString(5);
            channel.content = IdentifyChannel8(name, r.tag);

            channel.scale_factor = r.ReadFloat();
            channel.scale_offset = r.ReadFloat();
            // Documented as Uint32; stored as Int32 like the 16-bit channels
            channel.start_angle = r.ReadInt32();
            channel.angle_step = r.ReadUint16();
            channel.num_data = r.ReadUint16();

            if (channel.num_data > kMaxPointsPerScan) {
                g_log.Warn("[{}] Channel {} data count {} exceeds maximum {}", r.tag, name, channel.num_data,
                           kMaxPointsPerScan);
                return make_error_code(ErrorCode::kProtocolError);
            }

            if (!r.HasBytes(channel.num_data)) {
                g_log.Warn("[{}] Truncated 8-bit channel {} data: need {} bytes, have {}", r.tag, name,
                           channel.num_data, r.Remaining());
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            channel.data.resize(channel.num_data);
            for (std::uint16_t i = 0; i < channel.num_data; i++) {
                channel.data[i] = r.ReadUint8();
            }

            out.channels_8bit.push_back(std::move(channel));
        }

        return {};
    }

    std::error_code ScanDataParser::ParsePosition(Reader &r, ScanData &out) {
        // YRotation(4); no reserved prefix on the LMS4000
        if (!r.HasBytes(4)) {
            g_log.Warn("[{}] Truncated position block, pos={}", r.tag, r.pos);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        out.y_rotation = r.ReadFloat();

        return {};
    }

    std::error_code ScanDataParser::ParseDeviceName(Reader &r, ScanData &out) {
        // NameFlag(2) [NameLength(2) Name(var)]
        if (!r.HasBytes(2)) {
            g_log.Warn("[{}] Truncated device name flag, pos={}", r.tag, r.pos);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        const auto name_flag = r.ReadUint16();
        out.has_device_name = (name_flag == 1);

        if (out.has_device_name) {
            if (!r.HasBytes(2)) {
                g_log.Warn("[{}] Truncated device name length, pos={}", r.tag, r.pos);
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            const auto name_len = r.ReadUint16();
            if (name_len > 16) {
                g_log.Warn("[{}] Device name length {} exceeds max 16", r.tag, name_len);
                return make_error_code(ErrorCode::kProtocolError);
            }

            if (!r.HasBytes(name_len)) {
                g_log.Warn("[{}] Truncated device name string, pos={}", r.tag, r.pos);
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            out.device_name = r.ReadString(name_len);
        }

        return {};
    }

    std::error_code ScanDataParser::ParseTimestamp(Reader &r, ScanData &out) {
        // TimeFlag(2) [Year(2) Month(1) Day(1) Hour(1) Minute(1) Second(1) Microsecond(4)]
        if (!r.HasBytes(2)) {
            g_log.Warn("[{}] Truncated timestamp flag, pos={}", r.tag, r.pos);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        const auto time_flag = r.ReadUint16();
        out.has_timestamp = (time_flag == 1);

        if (out.has_timestamp) {
            if (!r.HasBytes(11)) {
                g_log.Warn("[{}] Truncated timestamp data, pos={}", r.tag, r.pos);
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            out.timestamp.year = r.ReadUint16();
            out.timestamp.month = r.ReadUint8();
            out.timestamp.day = r.ReadUint8();
            out.timestamp.hour = r.ReadUint8();
            out.timestamp.minute = r.ReadUint8();
            out.timestamp.second = r.ReadUint8();
            out.timestamp.microsecond = r.ReadUint32();
        }

        return {};
    }

    ChannelContent16 ScanDataParser::IdentifyChannel16(const std::string &name, std::string_view tag) {
        if (name == "DIST1") {
            return ChannelContent16::kDist1;
        }
        if (name == "RSSI1") {
            return ChannelContent16::kRssi1;
        }
        if (name == "REFL1") {
            return ChannelContent16::kRefl1;
        }
        if (name == "ANGL1") {
            return ChannelContent16::kAngl1;
        }

        g_log.Warn("[{}] Unknown 16-bit channel name: '{}'", tag, name);
        return ChannelContent16::kUnknown;
    }

    ChannelContent8 ScanDataParser::IdentifyChannel8(const std::string &name, std::string_view tag) {
        if (name == "QLTY1") {
            return ChannelContent8::kQlty1;
        }

        g_log.Warn("[{}] Unknown 8-bit channel name: '{}'", tag, name);
        return ChannelContent8::kUnknown;
    }
} // namespace lms4xxx
