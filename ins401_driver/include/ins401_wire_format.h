/// @file ins401_wire_format.h
/// @brief INS401 wire-format single source of truth: payload lengths, POD
/// record structs and the offset-based parsers, shared VERBATIM between the
/// driver (ins401_receiver) and the offline DataConverter (parse/).
/// Self-contained on purpose: no Eigen, no logging, standard headers only.

#ifndef INS401_WIRE_FORMAT_H
#define INS401_WIRE_FORMAT_H

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>


namespace INS401 {
// Payload lengths (bytes) of the fixed-size Aceinna packets
inline constexpr std::size_t GNSS_SOLUTION_PACKET_LENGTH = 77;

inline constexpr std::size_t INS_SOLUTION_PACKET_LENGTH = 110;

inline constexpr std::size_t DIAGNOSTIC_MESSAGE_LENGTH = 22;

inline constexpr std::size_t RAW_IMU_DATA_LENGTH = 30;


// Parsed GNSS position and velocity solution.
struct GNSSSolutionData {
	std::uint16_t gps_week;
	std::uint32_t gps_millisecs;  // ms
	std::uint8_t position_type;	  // 0: Invalid; 1: SPP; 2: RTD; 3: INS_PROPOGATED; 4: RTK_FIXED; 5: RTK_FLOAT
	double latitude;			  // deg
	double longitude;			  // deg
	double height;				  // m
	float latitude_std;			  // m
	float longitude_std;		  // m
	float height_std;			  // m
	std::uint8_t num_of_SVs;
	std::uint8_t num_of_SVs_in_solution;
	float hdop;
	float diffage;		  // s
	float north_vel;	  // m/s
	float east_vel;		  // m/s
	float up_vel;		  // m/s
	float north_vel_std;  // m/s
	float east_vel_std;	  // m/s
	float up_vel_std;	  // m/s
};


// Parsed INS navigation solution.
struct INSSolutionData {
	std::uint16_t gps_week;
	std::uint32_t gps_millisecs;  // ms
	std::uint8_t ins_status;
	// 0: Invalid; 1: INS_ALIGNING; 2: INS_HIGH_VARIANCE; 3: INS_SOLUTION_GOOD; 4: INS_SOLUTION_FREE; 5: INS_ALIGNMENT_COMPLETE
	std::uint8_t ins_position_type;
	// 0: Invalid; 1: SPP/INS; 2: RTD/INS; 3: INS_PROPOGATED; 4: RTK_FIXED/INS; 5: RTK_FLOAT/INS
	double latitude;		 // deg
	double longitude;		 // deg
	double height;			 // m
	float north_vel;		 // m/s
	float east_vel;			 // m/s
	float up_vel;			 // m/s
	float longitudinal_vel;	 // m/s
	float lateral_vel;		 // m/s
	float roll;				 // deg
	float pitch;			 // deg
	float heading;			 // deg
	float latitude_std;		 // m
	float longitude_std;	 // m
	float height_std;		 // m
	float north_vel_std;	 // m/s
	float east_vel_std;		 // m/s
	float up_vel_std;		 // m/s
	float long_vel_std;		 // m/s
	float lat_vel_std;		 // m/s
	float roll_std;			 // deg
	float pitch_std;		 // deg
	float heading_std;		 // deg
	std::uint16_t continent_id;
	// -2: ID_NONE; -1: ID_ERROR; 0: ID_UNKNOWN; 1: ID_AISA; 2: ID_EUROPE; 3: ID_OCEANIA; 4: ID_AFRICA; 5: ID_NORTHAMERICA; 6:
	// ID_SOUTHAMERICA; 7: ID_ANTARCTICA
};


// Device diagnostic and health status.
struct DiagnosticMessage {
	std::uint16_t gps_week;
	std::uint32_t gps_millisecs;		// ms
	std::array<int, 32> device_status;	// Refer to the Table 7 in manual
	float imu_temperature;				// ℃
	float mcu_temperature;				// ℃
	float gnss_chip_temperature;		// ℃
};


// Raw IMU accelerometer and gyroscope measurements.
struct RawIMUData {
	std::uint16_t gps_week;
	std::uint32_t gps_millisecs;  // ms
	float acc_x;				  // m/s^2
	float acc_y;				  // m/s^2
	float acc_z;				  // m/s^2
	float gyro_x;				  // deg/s
	float gyro_y;				  // deg/s
	float gyro_z;				  // deg/s
};


// Offset-based little-endian payload parsers (the packet CRC has already been
// verified by the caller).
inline GNSSSolutionData ParseGNSSSolutionPayload(const std::uint8_t *payload) {
	GNSSSolutionData gnss{};
	std::memcpy(&gnss.gps_week, payload, sizeof(std::uint16_t));
	std::memcpy(&gnss.gps_millisecs, payload + 2, sizeof(std::uint32_t));
	gnss.position_type = payload[6];
	std::memcpy(&gnss.latitude, payload + 7, sizeof(double));
	std::memcpy(&gnss.longitude, payload + 15, sizeof(double));
	std::memcpy(&gnss.height, payload + 23, sizeof(double));
	std::memcpy(&gnss.latitude_std, payload + 31, sizeof(float));
	std::memcpy(&gnss.longitude_std, payload + 35, sizeof(float));
	std::memcpy(&gnss.height_std, payload + 39, sizeof(float));
	gnss.num_of_SVs = payload[43];
	gnss.num_of_SVs_in_solution = payload[44];
	std::memcpy(&gnss.hdop, payload + 45, sizeof(float));
	std::memcpy(&gnss.diffage, payload + 49, sizeof(float));
	std::memcpy(&gnss.north_vel, payload + 53, sizeof(float));
	std::memcpy(&gnss.east_vel, payload + 57, sizeof(float));
	std::memcpy(&gnss.up_vel, payload + 61, sizeof(float));
	std::memcpy(&gnss.north_vel_std, payload + 65, sizeof(float));
	std::memcpy(&gnss.east_vel_std, payload + 69, sizeof(float));
	std::memcpy(&gnss.up_vel_std, payload + 73, sizeof(float));
	return gnss;
}


inline INSSolutionData ParseINSSolutionPayload(const std::uint8_t *payload) {
	INSSolutionData ins{};
	std::memcpy(&ins.gps_week, payload, sizeof(std::uint16_t));
	std::memcpy(&ins.gps_millisecs, payload + 2, sizeof(std::uint32_t));
	ins.ins_status = payload[6];
	ins.ins_position_type = payload[7];
	std::memcpy(&ins.latitude, payload + 8, sizeof(double));
	std::memcpy(&ins.longitude, payload + 16, sizeof(double));
	std::memcpy(&ins.height, payload + 24, sizeof(double));
	std::memcpy(&ins.north_vel, payload + 32, sizeof(float));
	std::memcpy(&ins.east_vel, payload + 36, sizeof(float));
	std::memcpy(&ins.up_vel, payload + 40, sizeof(float));
	std::memcpy(&ins.longitudinal_vel, payload + 44, sizeof(float));
	std::memcpy(&ins.lateral_vel, payload + 48, sizeof(float));
	std::memcpy(&ins.roll, payload + 52, sizeof(float));
	std::memcpy(&ins.pitch, payload + 56, sizeof(float));
	std::memcpy(&ins.heading, payload + 60, sizeof(float));
	std::memcpy(&ins.latitude_std, payload + 64, sizeof(float));
	std::memcpy(&ins.longitude_std, payload + 68, sizeof(float));
	std::memcpy(&ins.height_std, payload + 72, sizeof(float));
	std::memcpy(&ins.north_vel_std, payload + 76, sizeof(float));
	std::memcpy(&ins.east_vel_std, payload + 80, sizeof(float));
	std::memcpy(&ins.up_vel_std, payload + 84, sizeof(float));
	std::memcpy(&ins.long_vel_std, payload + 88, sizeof(float));
	std::memcpy(&ins.lat_vel_std, payload + 92, sizeof(float));
	std::memcpy(&ins.roll_std, payload + 96, sizeof(float));
	std::memcpy(&ins.pitch_std, payload + 100, sizeof(float));
	std::memcpy(&ins.heading_std, payload + 104, sizeof(float));
	std::memcpy(&ins.continent_id, payload + 108, sizeof(std::uint16_t));
	return ins;
}


inline DiagnosticMessage ParseDiagnosticPayload(const std::uint8_t *payload) {
	DiagnosticMessage diag{};
	std::memcpy(&diag.gps_week, payload, sizeof(std::uint16_t));
	std::memcpy(&diag.gps_millisecs, payload + 2, sizeof(std::uint32_t));
	std::uint32_t status_value;
	std::memcpy(&status_value, payload + 6, sizeof(std::uint32_t));
	const std::bitset<32> bs(status_value);
	for (int i = 0; i < 32; ++i) {
		diag.device_status[i] = bs[i];
	}
	std::memcpy(&diag.imu_temperature, payload + 10, sizeof(float));
	std::memcpy(&diag.mcu_temperature, payload + 14, sizeof(float));
	std::memcpy(&diag.gnss_chip_temperature, payload + 18, sizeof(float));
	return diag;
}


inline RawIMUData ParseRawIMUPayload(const std::uint8_t *payload) {
	RawIMUData imu{};
	std::memcpy(&imu.gps_week, payload, sizeof(std::uint16_t));
	std::memcpy(&imu.gps_millisecs, payload + 2, sizeof(std::uint32_t));
	std::memcpy(&imu.acc_x, payload + 6, sizeof(float));
	std::memcpy(&imu.acc_y, payload + 10, sizeof(float));
	std::memcpy(&imu.acc_z, payload + 14, sizeof(float));
	std::memcpy(&imu.gyro_x, payload + 18, sizeof(float));
	std::memcpy(&imu.gyro_y, payload + 22, sizeof(float));
	std::memcpy(&imu.gyro_z, payload + 26, sizeof(float));
	return imu;
}
}  // namespace INS401

#endif	// INS401_WIRE_FORMAT_H
