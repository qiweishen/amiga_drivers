#pragma once

#include <cstdint>
#include <string>
#include <map>



// LSB, MSB type
enum ENDIAN_TYPE {
	LSB,
	MSB
};


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

static constexpr uint16_t DIAGNOSTIC_MESSAGE_ID = 0x0a05;
static constexpr size_t DIAGNOSTIC_MESSAGE_LENGTH = 22;
static constexpr uint16_t GNSSCHIP_DIAGNOSTIC_MESSAGE_ID = 0x6664;
static constexpr size_t GNSSCHIP_DIAGNOSTIC_MESSAGE_LENGTH = 5;
static constexpr uint16_t DM_EXTENT_MESSAGE_ID = 0x4D44;
static constexpr size_t DM_EXTENT_MESSAGE_LENGTH = 8;

static constexpr uint16_t COMMAND_START = 0x5555;
static constexpr uint16_t REQUEST_INFO_COMMAND = 0xcc01; // Get the device information


// INS401 device information structure
struct DeviceInfo {
	std::string interface_name;
	std::string mac_address;
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


struct GNSSSolutionData {
	uint16_t gps_week;
	uint32_t gps_millisecs; // ms
	uint8_t position_type;
	double latitude; // deg
	double longitude;
	double height; // m
	float latitude_std; // m
	float longitude_std;
	float height_std;
	uint8_t num_of_SVs;
	uint8_t num_of_SVs_in_solution;
	float hdop;
	float diffage; // s
	float north_vel; // m/s
	float east_vel;
	float up_vel;
	float north_vel_std;
	float east_vel_std;
	float up_vel_std;
};


struct RawIMUData {
	uint16_t gps_week;
	uint32_t gps_millisecs; // ms
	float acc_x; // m/s²
	float acc_y;
	float acc_z;
	float gyro_x; // deg/s
	float gyro_y;
	float gyro_z;
};


struct INSSolutionData {
	// TODO
};


// NTRIP mount point structure
struct MountPoint {
	std::string mountpoint;
	std::string city;
	std::string data_format;
	std::string format_details;
	int carrier;
	std::string nav_system;
	std::string network;
	std::string country;
	double latitude;
	double longitude;
	int nmea;
	int solution;
	std::string generator;
	std::string compression;
	std::string authentication;
	int fee;
	int bitrate;
};


// HTTP response structure
struct HTTPResponse {
	int status_code;
	std::string status_text;
	std::map<std::string, std::string> headers;
	std::string body;
	bool is_chunked;
};
