/// @file data_type.h
/// @brief Central data structures for INS401 protocol messages, IMU/GNSS data, and shared utilities.

#ifndef DATA_TYPE_H
#define DATA_TYPE_H

#include <Eigen/Core>
#include <cstdint>
#include <string>


// Endianness selection for conversion helpers.
enum class EndianType {
    LSB,
    MSB
};


// Configurations
struct Config {
    std::string output_directory;
    bool enable_logging;
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

    std::string data_folder_path;
    std::string timestamp;
};


// Parsed GNSS position and velocity solution.
struct GNSSSolutionData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs; // ms
    std::uint8_t position_type; // 0: Invalid; 1: SPP; 2: RTD; 3: INS_PROPOGATED; 4: RTK_FIXED; 5: RTK_FLOAT
    double latitude; // deg
    double longitude; // deg
    double height; // m
    float latitude_std; // m
    float longitude_std; // m
    float height_std; // m
    std::uint8_t num_of_SVs;
    std::uint8_t num_of_SVs_in_solution;
    float hdop;
    float diffage; // s
    float north_vel; // m/s
    float east_vel; // m/s
    float up_vel; // m/s
    float north_vel_std; // m/s
    float east_vel_std; // m/s
    float up_vel_std; // m/s
};


// Parsed INS navigation solution.
struct INSSolutionData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs; // ms
    std::uint8_t ins_status;
    // 0: Invalid; 1: INS_ALIGNING; 2: INS_HIGH_VARIANCE; 3: INS_SOLUTION_GOOD; 4: INS_SOLUTION_FREE; 5: INS_ALIGNMENT_COMPLETE
    std::uint8_t ins_position_type;
    // 0: Invalid; 1: SPP/INS; 2: RTD/INS; 3: INS_PROPOGATED; 4: RTK_FIXED/INS; 5: RTK_FLOAT/INS
    double latitude; // deg
    double longitude; // deg
    double height; // m
    float north_vel; // m/s
    float east_vel; // m/s
    float up_vel; // m/s
    float longitudinal_vel; // m/s
    float lateral_vel; // m/s
    float roll; // deg
    float pitch; // deg
    float heading; // deg
    float latitude_std; // m
    float longitude_std; // m
    float height_std; // m
    float north_vel_std; // m/s
    float east_vel_std; // m/s
    float up_vel_std; // m/s
    float long_vel_std; // m/s
    float lat_vel_std; // m/s
    float roll_std; // deg
    float pitch_std; // deg
    float heading_std; // deg
    std::uint16_t continent_id;
    // -2: ID_NONE; -1: ID_ERROR; 0: ID_UNKNOWN; 1: ID_AISA; 2: ID_EUROPE; 3: ID_OCEANIA; 4: ID_AFRICA; 5: ID_NORTHAMERICA; 6: ID_SOUTHAMERICA; 7: ID_ANTARCTICA
};


// Device diagnostic and health status.
struct DiagnosticMessage {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs; // ms
    std::array<int, 32> device_status; // Refer to the Table 7 in manual
    float imu_temperature; // ℃
    float mcu_temperature; // ℃
    float gnss_chip_temperature; // ℃
};


// Raw IMU accelerometer and gyroscope measurements.
struct RawIMUData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs; // ms
    float acc_x; // m/s^2
    float acc_y; // m/s^2
    float acc_z; // m/s^2
    float gyro_x; // deg/s
    float gyro_y; // deg/s
    float gyro_z; // deg/s
};


// IMU data with Eigen vectors for mathematical operations.
struct ImuData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs; // ms
    Eigen::Vector3d accel; // m/s^2
    Eigen::Vector3d gyro; // deg/s
};


// GNSS data with Eigen vectors for mathematical operations.
struct GnssData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs; // ms
    std::uint8_t position_type; // same with the above
    Eigen::Vector3d enu_vel; // east, north, up velocity (m/s)
    float latitude_std; // m
    float longitude_std; // m
};


// Convert GPS week + time-of-week (milliseconds) to a continuous seconds value.
inline double GpsWeekTowToSec(std::uint16_t week, std::uint32_t millisecs) {
    return static_cast<double>(week) * 604800.0 + static_cast<double>(millisecs) * 0.001;
}


// Convert RawIMUData to ImuData with Eigen vectors.
inline ImuData ToImuData(const RawIMUData &raw) {
    ImuData d;
    d.gps_week = raw.gps_week;
    d.gps_millisecs = raw.gps_millisecs;
    d.accel = Eigen::Vector3d(raw.acc_x, raw.acc_y, raw.acc_z);
    d.gyro = Eigen::Vector3d(raw.gyro_x, raw.gyro_y, raw.gyro_z);
    return d;
}


// Convert GNSSSolutionData to GnssData with Eigen vectors.
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

    /// Incrementally update running sums for O(1) sliding-window mean/variance computation.
    void Add(const ImuData &sample) {
        gyro_sum += sample.gyro;
        accel_sum += sample.accel;
        gyro_sq_sum += sample.gyro.squaredNorm();
        accel_sq_sum += sample.accel.squaredNorm();
    }

    /// Remove a sample's contribution when it slides out of the window.
    void Remove(const ImuData &sample) {
        gyro_sum -= sample.gyro;
        accel_sum -= sample.accel;
        gyro_sq_sum -= sample.gyro.squaredNorm();
        accel_sq_sum -= sample.accel.squaredNorm();
    }
};


#endif
