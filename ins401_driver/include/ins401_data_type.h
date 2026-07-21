/// @file ins401_data_type.h
/// @brief Central data structures for INS401 protocol messages, IMU/GNSS data, and shared utilities.

#ifndef INS401_DATA_TYPE_H
#define INS401_DATA_TYPE_H

#include <Eigen/Core>
#include <string>
#include "ins401_wire_format.h"


namespace INS401 {
// Endianness selection for conversion helpers.
enum class EndianType { LSB, MSB };


struct INSConfig {
	bool enable_rtk;
	std::string host;
	int port;
	std::string mount_point;
	bool use_vrs;
	std::string username;
	std::string password;
	bool enable_gnss_checking;
	double gnss_horizontal_std_threshold;
	double accel_gravity_threshold;
	double accel_var_threshold;
	double gyro_var_threshold;
	double gyro_mean_threshold_xy;
	double gyro_mean_threshold_z;
	double min_stationary_duration_s;
	double recompute_interval_s;
	int required_stable_count;
	double stability_threshold_deg;

	bool enable_logging = true;
	std::string data_folder_path;
	std::string timestamp;
};


// IMU data with Eigen vectors for mathematical operations.
struct ImuData {
	std::uint16_t gps_week{};
	std::uint32_t gps_millisecs{};	// ms
	Eigen::Vector3d accel;			// m/s^2
	Eigen::Vector3d gyro;			// deg/s
};


// GNSS data with Eigen vectors for mathematical operations.
struct GnssData {
	std::uint16_t gps_week{};
	std::uint32_t gps_millisecs{};	// ms
	std::uint8_t position_type{};	// same with the above
	Eigen::Vector3d enu_vel;		// east, north, up velocity (m/s)
	float latitude_std{};			// m
	float longitude_std{};			// m
};


// Convert GPS week + time-of-week (milliseconds) to a continuous seconds value.
inline double GpsWeekTowToSec(std::uint16_t week, std::uint32_t millisecs) {
	return static_cast<double>(week) * 604800.0 + static_cast<double>(millisecs) * 0.001;
}


inline ImuData ToImuData(const RawIMUData &raw) {
	ImuData d;
	d.gps_week = raw.gps_week;
	d.gps_millisecs = raw.gps_millisecs;
	d.accel = Eigen::Vector3d(raw.acc_x, raw.acc_y, raw.acc_z);
	d.gyro = Eigen::Vector3d(raw.gyro_x, raw.gyro_y, raw.gyro_z);
	return d;
}


inline GnssData ToGnssData(const GNSSSolutionData &raw) {
	GnssData d;
	d.gps_week = raw.gps_week;
	d.gps_millisecs = raw.gps_millisecs;
	d.position_type = raw.position_type;
	d.enu_vel = Eigen::Vector3d(raw.east_vel, raw.north_vel, raw.up_vel);
	d.latitude_std = raw.latitude_std;
	d.longitude_std = raw.longitude_std;
	return d;
}


// Sliding-window statistics for stationary detection (shared by real-time and post-processing).
struct ImuWindowStats {
	Eigen::Vector3d gyro_sum = Eigen::Vector3d::Zero();
	Eigen::Vector3d accel_sum = Eigen::Vector3d::Zero();
	double gyro_sq_sum = 0.0;
	double accel_sq_sum = 0.0;

	// Incrementally update running sums for O(1) sliding-window mean/variance computation.
	void Add(const ImuData &sample) {
		gyro_sum += sample.gyro;
		accel_sum += sample.accel;
		gyro_sq_sum += sample.gyro.squaredNorm();
		accel_sq_sum += sample.accel.squaredNorm();
	}

	// Remove a sample's contribution when it slides out of the window.
	void Remove(const ImuData &sample) {
		gyro_sum -= sample.gyro;
		accel_sum -= sample.accel;
		gyro_sq_sum -= sample.gyro.squaredNorm();
		accel_sq_sum -= sample.accel.squaredNorm();
	}
};
}  // namespace INS401

#endif
