#ifndef LMS4XXX_COLA_B_CODEC_H
#define LMS4XXX_COLA_B_CODEC_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>


namespace LMS4xxx {

	// CoLa B command types (3-byte ASCII identifiers)
	namespace CommandType {
		inline constexpr std::string_view kReadByName = "sRN";	  ///< Read variable
		inline constexpr std::string_view kWriteByName = "sWN";	  ///< Write variable
		inline constexpr std::string_view kMethodByName = "sMN";  ///< Invoke method
		inline constexpr std::string_view kEventByName = "sEN";	  ///< Subscribe/unsubscribe event
		inline constexpr std::string_view kReadAnswer = "sRA";	  ///< Read response
		inline constexpr std::string_view kWriteAnswer = "sWA";	  ///< Write response
		inline constexpr std::string_view kMethodAnswer = "sAN";  ///< Method response
		inline constexpr std::string_view kEventAnswer = "sEA";	  ///< Event subscription response
		inline constexpr std::string_view kEventNotify = "sSN";	  ///< Event notification (async)
	}  // namespace CommandType


	// STX header bytes
	inline constexpr std::array<std::uint8_t, 4> kCoLaBStx = { 0x02, 0x02, 0x02, 0x02 };

	// Minimum frame size: STX(4) + Length(4) + CmdType(3) + Space(1) + CS(1) = 13
	inline constexpr std::size_t kCoLaBMinFrameSize = 13;

	// Space separator byte used in CoLa B
	inline constexpr std::uint8_t kCoLaBSpace = 0x20;


	// Decoded CoLa B message
	struct CoLaBMessage {
		std::string command_type;			///< e.g., "sRA", "sSN", "sAN"
		std::string command_name;			///< e.g., "LMDscandata", "SetAccessMode"
		std::vector<std::uint8_t> payload;	///< Raw binary parameters after command name
	};


	// CoLa B binary protocol encoder and decoder
	class CoLaBCodec {
	public:
		CoLaBCodec() = default;

		// Build a complete CoLa B frame (STX + Length + Data + Checksum)
		[[nodiscard]] static std::vector<std::uint8_t> Encode(std::string_view command_type, std::string_view command_name,
															  const std::vector<std::uint8_t> &params = {});

		// Encode a 32-bit unsigned integer in big-endian
		[[nodiscard]] static std::array<std::uint8_t, 4> EncodeUint32(std::uint32_t value);

		// Encode a 16-bit unsigned integer in big-endian
		[[nodiscard]] static std::array<std::uint8_t, 2> EncodeUint16(std::uint16_t value);

		// Encode a 32-bit signed integer in big-endian
		[[nodiscard]] static std::array<std::uint8_t, 4> EncodeInt32(std::int32_t value);

		// Encode an IEEE 754 float in big-endian
		[[nodiscard]] static std::array<std::uint8_t, 4> EncodeFloat(float value);

		// Decode a CoLa B frame data section into a structured message
		[[nodiscard]] static std::error_code Decode(const std::uint8_t *data, std::size_t len, CoLaBMessage &msg);

		// Decode a big-endian uint32 from a buffer
		[[nodiscard]] static std::uint32_t DecodeUint32(const std::uint8_t *buf);

		// Decode a big-endian uint16 from a buffer
		[[nodiscard]] static std::uint16_t DecodeUint16(const std::uint8_t *buf);

		// Decode a big-endian int32 from a buffer
		[[nodiscard]] static std::int32_t DecodeInt32(const std::uint8_t *buf);

		// Decode a big-endian IEEE 754 float from a buffer
		[[nodiscard]] static float DecodeFloat(const std::uint8_t *buf);

		// Decode a fixed-length ASCII string from a buffer
		[[nodiscard]] static std::string DecodeString(const std::uint8_t *buf, std::size_t len);

		// Compute CRC8 checksum (XOR of all bytes)
		[[nodiscard]] static std::uint8_t ComputeChecksum(const std::uint8_t *data, std::size_t len);
	};

}  // namespace LMS4xxx

#endif	// LMS4XXX_COLA_B_CODEC_H
