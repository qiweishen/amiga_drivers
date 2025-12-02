/**
 * @file ins401_protocol.h
 * @brief INS401 communication protocol constants and utility functions.
 *
 * This header defines protocol-specific constants, message identifiers, and
 * utility functions for communicating with Aceinna INS401 devices over Ethernet.
 *
 * @author Qiwei
 * @date 2025
 *
 * @note All multi-byte values in the INS401 protocol use little-endian (LSB first)
 *       byte ordering unless otherwise specified.
 */

#pragma once

#include <array>
#include <cstddef>
#include <string>



/**
 * @defgroup ProtocolConstants Protocol Constants
 * @brief Core protocol constants for INS401 communication.
 * @{
 */

/// @brief Broadcast MAC address for device discovery (FF:FF:FF:FF:FF:FF).
const std::string BROADCAST_MAC = "FF:FF:FF:FF:FF:FF";

/// @brief Size of Aceinna packet header in bytes (preamble + message ID).
static constexpr std::size_t ACENINNA_PRE_AND_ID = 4;

/// @brief Aceinna packet header length in bytes (preamble + message ID + payload length).
static constexpr std::size_t ACENINNA_HEADER_LEN = 8;

/// @brief RTCM3 message preamble byte.
static constexpr std::uint8_t RTCM3PREAMB = 0xD3;

/// @brief Aceinna command/packet preamble (0x5555).
static constexpr std::uint16_t COMMAND_START = 0x5555;

/// @brief NMEA ASCII message start character ('$').
static constexpr std::uint8_t NEMA_ASCII_START = '$';

/** @} */  // end of ProtocolConstants


/**
 * @defgroup MessageIdentifiers Message Identifiers
 * @brief Message type identifiers and their associated payload lengths.
 *
 * Each message type in the INS401 protocol is identified by a unique 16-bit
 * message ID. The associated length constants define the expected payload
 * size for fixed-length messages.
 * @{
 */

/**
 * @name Output Messages (0x0Axx)
 * @brief Device-to-host output message identifiers.
 * @{
 */

/// @brief Raw IMU data message identifier.
static constexpr std::uint16_t RAW_IMU_DATA_MESSAGE_ID = 0x0A01;

/// @brief Raw IMU data payload length in bytes.
static constexpr std::size_t RAW_IMU_DATA_LENGTH = 30;

/// @brief GNSS solution packet message identifier.
static constexpr std::uint16_t GNSS_SOLUTION_PACKET_MESSAGE_ID = 0x0A02;

/// @brief GNSS solution packet payload length in bytes.
static constexpr std::size_t GNSS_SOLUTION_PACKET_LENGTH = 77;

/// @brief INS solution packet message identifier.
static constexpr std::uint16_t INS_SOLUTION_PACKET_MESSAGE_ID = 0x0A03;

/// @brief INS solution packet payload length in bytes.
static constexpr std::size_t INS_SOLUTION_PACKET_LENGTH = 110;

/// @brief Diagnostic message identifier.
static constexpr std::uint16_t DIAGNOSTIC_MESSAGE_ID = 0x0A05;

/// @brief Diagnostic message payload length in bytes.
static constexpr std::size_t DIAGNOSTIC_MESSAGE_LENGTH = 22;

/// @brief RTCM rover data message identifier.
static constexpr std::uint16_t RTCM_ROVER_DATA_MESSAGE_ID = 0x0A06;

/// @brief Maximum RTCM rover data payload length in bytes.
static constexpr std::size_t RTCM_ROVER_DATA_LENGTH_MAX = 1024;

/** @} */  // end of Output Messages

/**
 * @name Input Messages (0x0Bxx)
 * @brief Host-to-device input message identifiers.
 * @{
 */

/// @brief RTCM base station data message identifier.
static constexpr std::uint16_t RTCM_BASE_DATA_MESSAGE_ID = 0x0B02;

/** @} */  // end of Input Messages

/**
 * @name Command Messages (0xCCxx)
 * @brief Device command message identifiers.
 * @{
 */

/// @brief Request device information command identifier.
static constexpr std::uint16_t REQUEST_INFO_COMMAND = 0xCC01;

/** @} */  // end of Command Messages

/** @} */  // end of MessageIdentifiers


/**
 * @defgroup EndianUtilities Endianness Utilities
 * @brief Utilities for byte order conversion.
 * @{
 */

/**
 * @enum EndianType
 * @brief Specifies byte ordering for multi-byte value conversion.
 */
enum class EndianType {
	LSB,  ///< Little-endian: Least Significant Byte first.
	MSB	  ///< Big-endian: Most Significant Byte first.
};

/**
 * @brief Converts a 16-bit unsigned integer to a 2-byte array.
 *
 * Performs compile-time conversion of a 16-bit value to a byte array
 * with the specified endianness.
 *
 * @param[in] value 16-bit unsigned integer to convert.
 * @param[in] type  Byte ordering for the output array.
 *
 * @return 2-byte array containing the converted value.
 *
 * @par Example
 * @code
 * // Little-endian: 0x1234 -> {0x34, 0x12}
 * constexpr auto le = ConvertUint16ToUint8(0x1234, EndianType::LSB);
 *
 * // Big-endian: 0x1234 -> {0x12, 0x34}
 * constexpr auto be = ConvertUint16ToUint8(0x1234, EndianType::MSB);
 * @endcode
 *
 * @note This function is constexpr and can be evaluated at compile time.
 */
[[nodiscard]] constexpr std::array<std::uint8_t, 2> ConvertUint16ToUint8(std::uint16_t value, EndianType type) {
	return (type == EndianType::LSB) ? std::array<std::uint8_t, 2>{ static_cast<std::uint8_t>(value & 0xFF),
																	static_cast<std::uint8_t>((value >> 8) & 0xFF) }
									 : std::array<std::uint8_t, 2>{ static_cast<std::uint8_t>((value >> 8) & 0xFF),
																	static_cast<std::uint8_t>(value & 0xFF) };
}

/** @} */  // end of EndianUtilities


/**
 * @defgroup PrecomputedBytes Pre-computed Byte Arrays
 * @brief Compile-time converted message identifiers and commands.
 *
 * These constants provide ready-to-use byte array representations of
 * protocol values, eliminating runtime conversion overhead.
 *
 * All values are stored in little-endian (LSB first) format as required
 * by the INS401 protocol.
 * @{
 */

/// @brief Command preamble as byte array (0x5555 -> {0x55, 0x55}).
static constexpr auto COMMAND_START_BYTES = ConvertUint16ToUint8(COMMAND_START, EndianType::LSB);

/// @brief GNSS solution message ID as byte array.
static constexpr auto GNSS_SOLUTION_PACKET_MESSAGE_ID_BYTES = ConvertUint16ToUint8(GNSS_SOLUTION_PACKET_MESSAGE_ID, EndianType::LSB);

/// @brief INS solution message ID as byte array.
static constexpr auto INS_SOLUTION_PACKET_MESSAGE_ID_BYTES = ConvertUint16ToUint8(INS_SOLUTION_PACKET_MESSAGE_ID, EndianType::LSB);

/// @brief Diagnostic message ID as byte array.
static constexpr auto DIAGNOSTIC_MESSAGE_ID_BYTES = ConvertUint16ToUint8(DIAGNOSTIC_MESSAGE_ID, EndianType::LSB);

/// @brief Raw IMU data message ID as byte array.
static constexpr auto RAW_IMU_DATA_MESSAGE_ID_BYTES = ConvertUint16ToUint8(RAW_IMU_DATA_MESSAGE_ID, EndianType::LSB);

/// @brief RTCM rover data message ID as byte array.
static constexpr auto RTCM_ROVER_DATA_MESSAGE_ID_BYTES = ConvertUint16ToUint8(RTCM_ROVER_DATA_MESSAGE_ID, EndianType::LSB);

/// @brief RTCM base data message ID as byte array.
static constexpr auto RTCM_BASE_DATA_MESSAGE_ID_BYTES = ConvertUint16ToUint8(RTCM_BASE_DATA_MESSAGE_ID, EndianType::LSB);

/// @brief Request info command as byte array.
static constexpr auto REQUEST_INFO_COMMAND_BYTES = ConvertUint16ToUint8(REQUEST_INFO_COMMAND, EndianType::LSB);

/** @} */  // end of PrecomputedBytes
