#ifndef ORIENTATION_INITIALIZER_H
#define ORIENTATION_INITIALIZER_H

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <cstddef>
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
    double local_gravity = 0.0; // m/s^2
};


class StaticInitializer {
public:
    explicit StaticInitializer(const double &gravity, const std::vector<ImuData> &static_imu,
                               bool accel_points_with_gravity = true);

    ~StaticInitializer() = default;

    struct ImuAlignmentResult {
        Eigen::Matrix3d R_enu_from_imu = Eigen::Matrix3d::Identity();
        double roll = -99.0; // rad
        double pitch = -99.0; // rad
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


#endif //ORIENTATION_INITIALIZER_H
