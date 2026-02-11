#ifndef ORIENTATION_INITIALIZER_H
#define ORIENTATION_INITIALIZER_H

#include <eigen3/Eigen/Core>
#include <eigen3/Eigen/Geometry>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "data_type.h"


struct InitializationResult {
    std::uint32_t timestamp = 0;
    Eigen::Vector3d position = Eigen::Vector3d::Zero(); // ENU
    Eigen::Quaterniond orientation = Eigen::Quaterniond::Identity(); // Body to ENU
    Eigen::Vector3d velocity = Eigen::Vector3d::Zero(); // m/s
    Eigen::Vector3d gyro_bias = Eigen::Vector3d::Zero(); // deg/s
    Eigen::Vector3d accel_bias = Eigen::Vector3d::Zero(); // m/s^2
    double roll = -99.0; // rad
    double pitch = -99.0; // rad
    double yaw = -99.0; // rad
    double roll_std = 0.0; // rad
    double pitch_std = 0.0; // rad
    double yaw_std = 0.0; // rad
    double local_gravity = 0.0; // m/s^2
};


class StaticInitializer {
public:
    explicit StaticInitializer(const double &gravity, const std::vector<RawIMUData> &static_imu, double yaw = -99,
                               double yaw_std = 0.0, double accel_noise_std = 0.0,
                               bool accel_points_with_gravity = true);

    explicit StaticInitializer(const Eigen::Vector3d &gravity_enu, const std::vector<ImuData> &static_imu,
                               double yaw = -99, double yaw_std = 0.0, double accel_noise_std = 0.0,
                               bool accel_points_with_gravity = true);

    ~StaticInitializer() = default;

    struct ImuAlignmentResult {
        Eigen::Matrix3d R_enu_from_imu = Eigen::Matrix3d::Identity();
        double roll = -99.0; // rad
        double pitch = -99.0; // rad
        double yaw = -99.0; // rad
        double roll_std = 0.0; // rad
        double pitch_std = 0.0; // rad
        double yaw_std = 0.0; // rad
    };

    struct ImuBiasResult {
        Eigen::Vector3d gyro_bias = Eigen::Vector3d::Zero();
        Eigen::Vector3d accel_bias = Eigen::Vector3d::Zero();
    };

    ImuBiasResult ComputeImuBias() const;

    ImuAlignmentResult AlignImuWithGravity() const;

private:
    Eigen::Vector3d origin_gravity_enu_; // Gravity vector at the origin in ENU frame (m/s²)
    std::vector<ImuData> static_imu_; // IMU data during the static period for bias estimation.
    double yaw_;
    double yaw_std_;
    double accel_noise_std_;
    bool accel_points_with_gravity_; // true: accel points with gravity; false: accel opposite gravity.

    struct ImuMeanStats {
        Eigen::Vector3d accel_mean = Eigen::Vector3d::Zero();
        Eigen::Vector3d gyro_mean = Eigen::Vector3d::Zero();
        std::size_t sample_count = 0;
    };

    struct AlignmentCache {
        Eigen::Vector3d accel_dir = Eigen::Vector3d::Zero();
        Eigen::Vector3d accel_dir_aligned = Eigen::Vector3d::Zero();
        Eigen::Vector3d gravity_dir = Eigen::Vector3d::Zero();
        double accel_norm = 0.0;
        double gravity_norm = 0.0;
        bool valid = false;
    };

    void UpdateImuMeanStats();

    void UpdateAlignmentCache();

    ImuMeanStats imu_mean_stats_;
    AlignmentCache alignment_cache_;
};


class YawEstimator {
public:
    /// Estimate initial yaw from GNSS ENU velocity heading.
    /// Uses the first GNSS epochs after the stationary period that have sufficient horizontal speed.
    /// @param gnss_data  GNSS data (must already be time-filtered to start after stationary period).
    /// @param min_speed  Minimum horizontal speed (m/s) to consider a GNSS epoch valid for heading.
    /// @param max_epochs Maximum number of qualifying epochs to average.
    explicit YawEstimator(const std::vector<GnssData> &gnss_data,
                          double min_speed = 0.15,
                          int max_epochs = 5);

    ~YawEstimator() = default;

    struct YawResult {
        double yaw = 0.0; // rad, ENU convention: 0 = North, positive CCW
        double yaw_std = M_PI; // rad, large default = unknown
        bool valid = false;
    };

    YawResult EstimateYaw() const;

private:
    std::vector<GnssData> gnss_data_;
    double min_speed_;
    int max_epochs_;
};


#endif //ORIENTATION_INITIALIZER_H