#ifndef DATA_TYPE_H
#define DATA_TYPE_H

#include <Eigen/Core>


// Endianness selection for conversion helpers.
enum class EndianType {
    LSB,
    MSB
};


// Parsed GNSS position and velocity solution.
struct GNSSSolutionData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs;
    std::uint8_t position_type;
    double latitude;
    double longitude;
    double height;
    float latitude_std;
    float longitude_std;
    float height_std;
    std::uint8_t num_of_SVs;
    std::uint8_t num_of_SVs_in_solution;
    float hdop;
    float diffage;
    float north_vel;
    float east_vel;
    float up_vel;
    float north_vel_std;
    float east_vel_std;
    float up_vel_std;
};


// Parsed INS navigation solution.
struct INSSolutionData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs;
    std::uint8_t ins_status;
    std::uint8_t ins_position_type;
    double latitude;
    double longitude;
    double height;
    float north_vel;
    float east_vel;
    float up_vel;
    float longitudinal_vel;
    float lateral_vel;
    float roll;
    float pitch;
    float heading;
    float latitude_std;
    float longitude_std;
    float height_std;
    float north_vel_std;
    float east_vel_std;
    float up_vel_std;
    float long_vel_std;
    float lat_vel_std;
    float roll_std;
    float pitch_std;
    float heading_std;
    std::uint16_t continent_id;
};


// Device diagnostic and health status.
struct DiagnosticMessage {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs;
    std::array<int, 32> device_status;
    float imu_temperature;
    float mcu_temperature;
    float gnss_chip_temperature;
};


// Raw IMU accelerometer and gyroscope measurements.
struct RawIMUData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs;
    float acc_x;
    float acc_y;
    float acc_z;
    float gyro_x;
    float gyro_y;
    float gyro_z;
};


// IMU data with Eigen vectors for mathematical operations.
struct ImuData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs;
    Eigen::Vector3d accel; // m/s^2
    Eigen::Vector3d gyro; // deg/s
};


// GNSS data with Eigen vectors for mathematical operations.
struct GnssData {
    std::uint16_t gps_week;
    std::uint32_t gps_millisecs;
    std::uint8_t position_type;
    Eigen::Vector3d enu_vel; // east, north, up velocity (m/s)
    float latitude_std;
    float longitude_std;
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


#endif
