#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "tool.h"



// LSB, MSB type
enum ENDIAN_TYPE { LSB, MSB };


// Protocol constant value
static constexpr size_t ETH_HEADER_LEN = 14;
static constexpr size_t ACENINNA_HEADER_LEN = 8;
static constexpr uint8_t RTCM3PREAMB = 0xD3;

static constexpr uint16_t GNSS_SOLUTION_PACKET_ID = 0x0a02;
static constexpr size_t GNSS_SOLUTION_PACKET_LENGTH = 77;
static constexpr uint16_t INS_Solution_PACKET_ID = 0x0a03;
static constexpr size_t INS_Solution_PACKET_LENGTH = 110;
static constexpr uint16_t RAW_IMU_DATA_PACKET_ID = 0x0a01;
static constexpr size_t RAW_IMU_DATA_PACKET_LENGTH = 30;

static constexpr uint16_t RTCM_BASE_DATA_MESSAGE_ID = 0x0b02;

static constexpr uint16_t DIAGNOSTIC_MESSAGE_ID = 0x0a05;
static constexpr size_t DIAGNOSTIC_MESSAGE_LENGTH = 22;
static constexpr uint16_t GNSSCHIP_DIAGNOSTIC_MESSAGE_ID = 0x6664;

static constexpr size_t GNSSCHIP_DIAGNOSTIC_MESSAGE_LENGTH = 5;
static constexpr uint16_t DM_EXTENT_MESSAGE_ID = 0x4D44;
static constexpr size_t DM_EXTENT_MESSAGE_LENGTH = 8;

const std::string BROADCAST_MAC = "FF:FF:FF:FF:FF:FF";
static constexpr uint16_t COMMAND_START = 0x5555;
static constexpr uint16_t REQUEST_INFO_COMMAND = 0xcc01;  // Get the device information

static constexpr auto GNSS_SOLUTION_PACKET_ID_BYTES = Tool::Ethernet::ConvertUint16ToUint8(GNSS_SOLUTION_PACKET_ID, LSB);
static constexpr auto INS_Solution_PACKET_ID_BYTES = Tool::Ethernet::ConvertUint16ToUint8(INS_Solution_PACKET_ID, LSB);
static constexpr auto RAW_IMU_DATA_PACKET_ID_BYTES = Tool::Ethernet::ConvertUint16ToUint8(RAW_IMU_DATA_PACKET_ID, LSB);
static constexpr auto RTCM_BASE_DATA_MESSAGE_ID_BYTES = Tool::Ethernet::ConvertUint16ToUint8(RTCM_BASE_DATA_MESSAGE_ID, LSB);
static constexpr auto DIAGNOSTIC_MESSAGE_ID_BYTES = Tool::Ethernet::ConvertUint16ToUint8(DIAGNOSTIC_MESSAGE_ID, LSB);
static constexpr auto GNSSCHIP_DIAGNOSTIC_MESSAGE_ID_BYTES = Tool::Ethernet::ConvertUint16ToUint8(GNSSCHIP_DIAGNOSTIC_MESSAGE_ID, LSB);
static constexpr auto DM_EXTENT_MESSAGE_ID_BYTES = Tool::Ethernet::ConvertUint16ToUint8(DM_EXTENT_MESSAGE_ID, LSB);
static constexpr auto COMMAND_START_BYTES = Tool::Ethernet::ConvertUint16ToUint8(COMMAND_START, LSB);
static constexpr auto REQUEST_INFO_COMMAND_BYTES = Tool::Ethernet::ConvertUint16ToUint8(REQUEST_INFO_COMMAND, LSB);


// INS401 device information structure
struct DeviceInfo {
	std::string interface_name;
	std::string mac_address;
	std::string localhost_mac_address;
	std::string product = "INS401";
	std::string part_number;
	std::string serial_number;
	std::string hardware_version;
	std::string imu_serial_number;
	std::string firmware_version;
	std::string bootloader_version;
	std::string imu_firmware_version;
	std::string gnss_chip_firmware_version;
};


// HTTP response structure
struct HTTPResponse {
	int status_code;
	std::string status_text;
	std::map<std::string, std::string> headers;
	std::string body;
	bool is_chunked;
};
