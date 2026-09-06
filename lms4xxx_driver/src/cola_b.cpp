#include "cola_b.h"

#include "byte_util.h"

#include <cstring>

#include "error.h"
#include "logger.h"
#include "utility.h"


namespace {
    constexpr std::string_view kModule = "LMS4xxx";
    common::DriverLog g_log{std::string(kModule)};
} // namespace


namespace lms4xxx {
    const char *SopasErrorText(std::uint16_t code) {
        switch (code) {
            case 0:
                return "no error";
            case 1:
                return "wrong user level, access to method not allowed";
            case 2:
                return "unknown method index";
            case 3:
                return "unknown variable index";
            case 4:
                return "local condition failed (value out of range)";
            case 5:
                return "invalid data";
            case 6:
                return "unknown error";
            case 7:
                return "buffer overflow";
            case 8:
                return "buffer underflow";
            case 9:
                return "unknown type";
            case 10:
                return "variable is read-only";
            case 11:
                return "unknown command for name server";
            case 12:
                return "unknown CoLa command";
            case 13:
                return "server busy";
            case 14:
                return "flex array out of bounds";
            case 15:
                return "unknown event index";
            case 16:
                return "CoLa A value overflow";
            case 17:
                return "CoLa A invalid character";
            case 18:
                return "OSAI no message";
            case 19:
                return "OSAI no answer message";
            case 20:
                return "internal firmware error";
            case 21:
                return "hub address corrupted";
            case 22:
                return "hub address decoding";
            case 23:
                return "hub address exceeded";
            case 24:
                return "hub address blank expected";
            case 25:
                return "async methods suppressed";
            case 26:
                return "complex arrays not supported";
            default:
                return "unknown SOPAS error code";
        }
    }


    std::vector<std::uint8_t> ColaBCodec::Encode(std::string_view command_type, std::string_view command_name,
                                                 const std::vector<std::uint8_t> &params) {
        // Data: CommandType 0x20 CommandName [0x20 Params]
        std::vector<std::uint8_t> data;
        data.reserve(command_type.size() + 1 + command_name.size() + (params.empty() ? 0 : 1 + params.size()));

        data.insert(data.end(), command_type.begin(), command_type.end());
        data.push_back(kColaBSpace);
        data.insert(data.end(), command_name.begin(), command_name.end());

        if (!params.empty()) {
            data.push_back(kColaBSpace);
            data.insert(data.end(), params.begin(), params.end());
        }

        // Frame: STX(4) Length(4) Data Checksum(1)
        const auto data_len = static_cast<std::uint32_t>(data.size());
        const auto len_bytes = EncodeUint32(data_len);
        const auto checksum = ComputeChecksum(data.data(), data.size());

        std::vector<std::uint8_t> frame;
        frame.reserve(4 + 4 + data.size() + 1);

        frame.insert(frame.end(), kColaBStx.begin(), kColaBStx.end());
        frame.insert(frame.end(), len_bytes.begin(), len_bytes.end());
        frame.insert(frame.end(), data.begin(), data.end());
        frame.push_back(checksum);

        return frame;
    }


    std::array<std::uint8_t, 4> ColaBCodec::EncodeUint32(std::uint32_t value) {
        std::array<std::uint8_t, 4> out{};
        common::ByteUtil::StoreBigU32(out.data(), value);
        return out;
    }


    std::array<std::uint8_t, 2> ColaBCodec::EncodeUint16(std::uint16_t value) {
        return {
            {
                static_cast<std::uint8_t>((value >> 8) & 0xFF),
                static_cast<std::uint8_t>(value & 0xFF),
            }
        };
    }


    std::array<std::uint8_t, 4> ColaBCodec::EncodeInt32(std::int32_t value) {
        return EncodeUint32(static_cast<std::uint32_t>(value));
    }


    std::array<std::uint8_t, 4> ColaBCodec::EncodeFloat(float value) {
        std::uint32_t raw;
        std::memcpy(&raw, &value, sizeof(float));
        return EncodeUint32(raw);
    }


    std::error_code ColaBCodec::Decode(const std::uint8_t *data, std::size_t len, ColaBMessage &msg,
                                       std::string_view tag) {
        // CommandType(3) + Space(1)
        if (len < 4) {
            g_log.Warn("[{}] Frame data too short: {} bytes", tag, len);
            return make_error_code(ErrorCode::kFrameTooShort);
        }

        std::size_t pos = 0;

        msg.command_type.assign(reinterpret_cast<const char *>(data + pos), 3);
        pos += 3;

        if (data[pos] != kColaBSpace) {
            g_log.Warn("[{}] Expected space after command type, got 0x{:02X}", tag, data[pos]);
            return make_error_code(ErrorCode::kProtocolError);
        }
        pos++;

        if (msg.command_type == CommandType::kErrorAnswer) {
            msg.command_name.clear();
            msg.payload.assign(data + pos, data + len);
            return {};
        }

        // Command name runs to the next space or the end
        std::size_t name_start = pos;
        while (pos < len && data[pos] != kColaBSpace) {
            pos++;
        }

        if (pos == name_start) {
            g_log.Warn("[{}] Empty command name", tag);
            return make_error_code(ErrorCode::kProtocolError);
        }

        msg.command_name.assign(reinterpret_cast<const char *>(data + name_start), pos - name_start);

        // Rest is payload, minus the separating space
        if (pos < len && data[pos] == kColaBSpace) {
            pos++;
        }

        if (pos < len) {
            msg.payload.assign(data + pos, data + len);
        } else {
            msg.payload.clear();
        }

        return {};
    }


    std::vector<std::uint8_t> ColaBCodec::ParamsOf(const std::vector<std::uint8_t> &frame, std::size_t name_len) {
        const std::size_t start = 8 + 3 + 1 + name_len + 1; // header + type + SP + name + SP
        if (frame.size() < 2 || start >= frame.size() - 1) {
            return {};
        }
        return {
            frame.begin() + static_cast<std::ptrdiff_t>(start),
            frame.begin() + static_cast<std::ptrdiff_t>(frame.size() - 1)
        };
    }


    std::uint32_t ColaBCodec::DecodeUint32(const std::uint8_t *buf) {
        return common::ByteUtil::LoadBigU32(buf);
    }


    std::uint16_t ColaBCodec::DecodeUint16(const std::uint8_t *buf) {
        return common::ByteUtil::LoadBigU16(buf);
    }


    std::int32_t ColaBCodec::DecodeInt32(const std::uint8_t *buf) {
        return static_cast<std::int32_t>(DecodeUint32(buf));
    }


    float ColaBCodec::DecodeFloat(const std::uint8_t *buf) {
        const std::uint32_t raw = DecodeUint32(buf);
        float result;
        std::memcpy(&result, &raw, sizeof(float));
        return result;
    }


    std::string ColaBCodec::DecodeString(const std::uint8_t *buf, std::size_t len) {
        return std::string(reinterpret_cast<const char *>(buf), len);
    }


    std::uint8_t ColaBCodec::ComputeChecksum(const std::uint8_t *data, std::size_t len) {
        std::uint8_t cs = 0;
        for (std::size_t i = 0; i < len; i++) {
            cs ^= data[i];
        }
        return cs;
    }
} // namespace lms4xxx
