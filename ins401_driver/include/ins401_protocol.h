/// @file ins401_protocol.h
/// @brief INS401 Ethernet protocol constants: message IDs, payload sizes, and precomputed byte arrays.

#ifndef INS401_PROTOCOL_H
#define INS401_PROTOCOL_H

#include <array>
#include <cstddef>
#include <string_view>

#include "ins401_data_type.h"


// Aceinna packet framing: header = [0x5555(2) | MsgID(2) | PayloadLen(4)], trailer = [CRC16(2)].
// ACEINNA_HEADER_LEN = preamble(2) + msg_id(2) + payload_len(4) = 8 bytes.
inline constexpr std::string_view BROADCAST_MAC = "FF:FF:FF:FF:FF:FF";

inline constexpr std::size_t ACEINNA_PRE_AND_ID = 4;

inline constexpr std::size_t ACEINNA_HEADER_LEN = 8;

inline constexpr std::uint8_t RTCM3PREAMB = 0xD3;

inline constexpr std::uint16_t COMMAND_START = 0x5555;

inline constexpr std::uint8_t NMEA_ASCII_START = '$';
// Output messages (device-to-host).
inline constexpr std::uint16_t RAW_IMU_DATA_MESSAGE_ID = 0x0A01;

inline constexpr std::size_t RAW_IMU_DATA_LENGTH = 30;

inline constexpr std::uint16_t GNSS_SOLUTION_PACKET_MESSAGE_ID = 0x0A02;

inline constexpr std::size_t GNSS_SOLUTION_PACKET_LENGTH = 77;

inline constexpr std::uint16_t INS_SOLUTION_PACKET_MESSAGE_ID = 0x0A03;

inline constexpr std::size_t INS_SOLUTION_PACKET_LENGTH = 110;

inline constexpr std::uint16_t DIAGNOSTIC_MESSAGE_ID = 0x0A05;

inline constexpr std::size_t DIAGNOSTIC_MESSAGE_LENGTH = 22;

inline constexpr std::uint16_t RTCM_ROVER_DATA_MESSAGE_ID = 0x0A06;

inline constexpr std::size_t RTCM_ROVER_DATA_LENGTH_MAX = 1024;

// Input messages (host-to-device).
inline constexpr std::uint16_t RTCM_BASE_DATA_MESSAGE_ID = 0x0B02;
// Command messages.
inline constexpr std::uint16_t REQUEST_INFO_COMMAND = 0xCC01;


[[nodiscard]] constexpr std::array<std::uint8_t, 2> ConvertUint16ToUint8(std::uint16_t value, EndianType type) {
	return (type == EndianType::LSB) ? std::array<std::uint8_t, 2>{ static_cast<std::uint8_t>(value & 0xFF),
																	static_cast<std::uint8_t>((value >> 8) & 0xFF) }
									 : std::array<std::uint8_t, 2>{ static_cast<std::uint8_t>((value >> 8) & 0xFF),
																	static_cast<std::uint8_t>(value & 0xFF) };
}


// Precomputed byte arrays (LSB first).
inline constexpr auto COMMAND_START_BYTES = ConvertUint16ToUint8(COMMAND_START, EndianType::LSB);

inline constexpr auto GNSS_SOLUTION_PACKET_MESSAGE_ID_BYTES = ConvertUint16ToUint8(GNSS_SOLUTION_PACKET_MESSAGE_ID, EndianType::LSB);

inline constexpr auto INS_SOLUTION_PACKET_MESSAGE_ID_BYTES = ConvertUint16ToUint8(INS_SOLUTION_PACKET_MESSAGE_ID, EndianType::LSB);

inline constexpr auto DIAGNOSTIC_MESSAGE_ID_BYTES = ConvertUint16ToUint8(DIAGNOSTIC_MESSAGE_ID, EndianType::LSB);

inline constexpr auto RAW_IMU_DATA_MESSAGE_ID_BYTES = ConvertUint16ToUint8(RAW_IMU_DATA_MESSAGE_ID, EndianType::LSB);

inline constexpr auto RTCM_ROVER_DATA_MESSAGE_ID_BYTES = ConvertUint16ToUint8(RTCM_ROVER_DATA_MESSAGE_ID, EndianType::LSB);

inline constexpr auto RTCM_BASE_DATA_MESSAGE_ID_BYTES = ConvertUint16ToUint8(RTCM_BASE_DATA_MESSAGE_ID, EndianType::LSB);

inline constexpr auto REQUEST_INFO_COMMAND_BYTES = ConvertUint16ToUint8(REQUEST_INFO_COMMAND, EndianType::LSB);


#endif
