#ifndef INS401_PROTOCOL_H
#define INS401_PROTOCOL_H

#include <array>
#include <cstddef>
#include <string>

#include "data_type.h"


// Core protocol constants.
const std::string BROADCAST_MAC = "FF:FF:FF:FF:FF:FF";

static constexpr std::size_t ACENINNA_PRE_AND_ID = 4;

static constexpr std::size_t ACENINNA_HEADER_LEN = 8;

static constexpr std::uint8_t RTCM3PREAMB = 0xD3;

static constexpr std::uint16_t COMMAND_START = 0x5555;

static constexpr std::uint8_t NEMA_ASCII_START = '$';
// Output messages (device-to-host).
static constexpr std::uint16_t RAW_IMU_DATA_MESSAGE_ID = 0x0A01;

static constexpr std::size_t RAW_IMU_DATA_LENGTH = 30;

static constexpr std::uint16_t GNSS_SOLUTION_PACKET_MESSAGE_ID = 0x0A02;

static constexpr std::size_t GNSS_SOLUTION_PACKET_LENGTH = 77;

static constexpr std::uint16_t INS_SOLUTION_PACKET_MESSAGE_ID = 0x0A03;

static constexpr std::size_t INS_SOLUTION_PACKET_LENGTH = 110;

static constexpr std::uint16_t DIAGNOSTIC_MESSAGE_ID = 0x0A05;

static constexpr std::size_t DIAGNOSTIC_MESSAGE_LENGTH = 22;

static constexpr std::uint16_t RTCM_ROVER_DATA_MESSAGE_ID = 0x0A06;

static constexpr std::size_t RTCM_ROVER_DATA_LENGTH_MAX = 1024;

// Input messages (host-to-device).
static constexpr std::uint16_t RTCM_BASE_DATA_MESSAGE_ID = 0x0B02;
// Command messages.
static constexpr std::uint16_t REQUEST_INFO_COMMAND = 0xCC01;


[[nodiscard]] constexpr std::array<std::uint8_t, 2> ConvertUint16ToUint8(std::uint16_t value, EndianType type) {
    return (type == EndianType::LSB)
               ? std::array<std::uint8_t, 2>{
                   static_cast<std::uint8_t>(value & 0xFF),
                   static_cast<std::uint8_t>((value >> 8) & 0xFF)
               }
               : std::array<std::uint8_t, 2>{
                   static_cast<std::uint8_t>((value >> 8) & 0xFF),
                   static_cast<std::uint8_t>(value & 0xFF)
               };
}


// Precomputed byte arrays (LSB first).
static constexpr auto COMMAND_START_BYTES = ConvertUint16ToUint8(COMMAND_START, EndianType::LSB);

static constexpr auto GNSS_SOLUTION_PACKET_MESSAGE_ID_BYTES = ConvertUint16ToUint8(
    GNSS_SOLUTION_PACKET_MESSAGE_ID, EndianType::LSB);

static constexpr auto INS_SOLUTION_PACKET_MESSAGE_ID_BYTES = ConvertUint16ToUint8(
    INS_SOLUTION_PACKET_MESSAGE_ID, EndianType::LSB);

static constexpr auto DIAGNOSTIC_MESSAGE_ID_BYTES = ConvertUint16ToUint8(DIAGNOSTIC_MESSAGE_ID, EndianType::LSB);

static constexpr auto RAW_IMU_DATA_MESSAGE_ID_BYTES = ConvertUint16ToUint8(RAW_IMU_DATA_MESSAGE_ID, EndianType::LSB);

static constexpr auto RTCM_ROVER_DATA_MESSAGE_ID_BYTES = ConvertUint16ToUint8(
    RTCM_ROVER_DATA_MESSAGE_ID, EndianType::LSB);

static constexpr auto RTCM_BASE_DATA_MESSAGE_ID_BYTES =
        ConvertUint16ToUint8(RTCM_BASE_DATA_MESSAGE_ID, EndianType::LSB);

static constexpr auto REQUEST_INFO_COMMAND_BYTES = ConvertUint16ToUint8(REQUEST_INFO_COMMAND, EndianType::LSB);


#endif
