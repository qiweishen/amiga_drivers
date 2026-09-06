#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "scan_data.h"


namespace lms4xxx {
    // LMDscandata telegram payload -> ScanData
    namespace ScanDataParser {
        // `tag`: instance log prefix
        std::error_code Parse(const std::uint8_t *data, std::size_t len, ScanData &out, std::string_view tag = {});

        // Sequential field reader; carries the log tag for parse warnings
        struct Reader {
            const std::uint8_t *data;
            std::size_t len;
            std::size_t pos = 0;
            std::string_view tag{};

            bool HasBytes(std::size_t n) const { return pos + n <= len; }
            std::size_t Remaining() const { return len - pos; }

            std::uint8_t ReadUint8();

            std::uint16_t ReadUint16();

            std::int32_t ReadInt32();

            std::uint32_t ReadUint32();

            float ReadFloat();

            std::string ReadString(std::size_t length);

            void Skip(std::size_t n);
        };

        std::error_code ParseDeviceInfo(Reader &r, ScanData &out);

        std::error_code ParseFrequency(Reader &r, ScanData &out);

        std::error_code ParseEncoder(Reader &r, ScanData &out);

        std::error_code ParseChannels16bit(Reader &r, ScanData &out);

        std::error_code ParseChannels8bit(Reader &r, ScanData &out);

        std::error_code ParsePosition(Reader &r, ScanData &out);

        std::error_code ParseDeviceName(Reader &r, ScanData &out);

        std::error_code ParseTimestamp(Reader &r, ScanData &out);

        // 5-byte channel name -> content id
        ChannelContent16 IdentifyChannel16(const std::string &name, std::string_view tag);

        ChannelContent8 IdentifyChannel8(const std::string &name, std::string_view tag);
    } // namespace ScanDataParser
} // namespace lms4xxx

