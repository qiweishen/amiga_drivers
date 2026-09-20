#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>


namespace lms4xxx {
    // 3-byte ASCII command types
    namespace CommandType {
        inline constexpr std::string_view kReadByName = "sRN"; ///< Read variable
        inline constexpr std::string_view kWriteByName = "sWN"; ///< Write variable
        inline constexpr std::string_view kMethodByName = "sMN"; ///< Invoke method
        inline constexpr std::string_view kEventByName = "sEN"; ///< Subscribe/unsubscribe event
        inline constexpr std::string_view kReadAnswer = "sRA"; ///< Read response
        inline constexpr std::string_view kWriteAnswer = "sWA"; ///< Write response
        inline constexpr std::string_view kMethodAnswer = "sAN"; ///< Method response
        inline constexpr std::string_view kEventAnswer = "sEA"; ///< Event subscription response
        inline constexpr std::string_view kEventNotify = "sSN"; ///< Event notification (async)
        inline constexpr std::string_view kErrorAnswer = "sFA"; ///< SOPAS error, payload = Uint16 code
    } // namespace CommandType


    // Manual table 279 (p.143)
    const char *SopasErrorText(std::uint16_t code);


    inline constexpr std::array<std::uint8_t, 4> kColaBStx = {0x02, 0x02, 0x02, 0x02};

    inline constexpr std::uint8_t kColaBSpace = 0x20;


    struct ColaBMessage {
        std::string command_type; ///< e.g., "sRA", "sSN", "sAN"
        std::string command_name; ///< e.g., "LMDscandata", "SetAccessMode"
        std::vector<std::uint8_t> payload; ///< Raw binary parameters after command name
    };

    // Subscription acknowledgements use the SAME name but carry one status
    // byte, not a scan payload. Never dispatch them to ScanDataParser.
    inline bool IsScanMessage(const ColaBMessage &message) {
        return message.command_name == "LMDscandata" &&
               (message.command_type == CommandType::kEventNotify ||
                message.command_type == CommandType::kReadAnswer);
    }


    // Frame: STX(4) + Length(4) + Data + CS(1); integers big-endian
    namespace ColaBCodec {
        std::vector<std::uint8_t> Encode(std::string_view command_type,
                                                std::string_view command_name,
                                                const std::vector<std::uint8_t> &params = {});

        std::array<std::uint8_t, 4> EncodeUint32(std::uint32_t value);

        std::array<std::uint8_t, 2> EncodeUint16(std::uint16_t value);

        std::array<std::uint8_t, 4> EncodeInt32(std::int32_t value);

        std::array<std::uint8_t, 4> EncodeFloat(float value);

        // `tag`: instance log prefix. An sFA frame decodes with an empty name and the code as payload
        std::error_code Decode(const std::uint8_t *data, std::size_t len, ColaBMessage &msg,
                                      std::string_view tag = {});

        // Parameter bytes of an encoded frame (after "<type> <name> ", before the checksum)
        std::vector<std::uint8_t> ParamsOf(const std::vector<std::uint8_t> &frame, std::size_t name_len);

        std::uint32_t DecodeUint32(const std::uint8_t *buf);

        std::uint16_t DecodeUint16(const std::uint8_t *buf);

        std::int32_t DecodeInt32(const std::uint8_t *buf);

        float DecodeFloat(const std::uint8_t *buf);

        std::string DecodeString(const std::uint8_t *buf, std::size_t len);

        // XOR of all bytes
        std::uint8_t ComputeChecksum(const std::uint8_t *data, std::size_t len);
    } // namespace ColaBCodec
} // namespace lms4xxx
