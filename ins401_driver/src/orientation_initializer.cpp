#include "orientation_initializer.h"

#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <limits>


StaticInitializer::StaticInitializer(const double &gravity, const std::vector<ImuData> &static_imu, bool accel_points_with_gravity)
    : origin_gravity_enu_(0.0, 0.0, -std::abs(gravity)), static_imu_(static_imu), accel_points_with_gravity_(accel_points_with_gravity) {
    UpdateImuMeanStats();
    UpdateAlignmentCache();
}


void StaticInitializer::UpdateImuMeanStats() {
    imu_mean_stats_ = ImuMeanStats();
    imu_mean_stats_.sample_count = static_imu_.size();
    if (imu_mean_stats_.sample_count == 0) {
        return;
    }

    for (const auto &imu: static_imu_) {
        imu_mean_stats_.accel_mean += imu.accel;
        imu_mean_stats_.gyro_mean += imu.gyro;
    }
    const double inv_n = 1.0 / static_cast<double>(imu_mean_stats_.sample_count);
    imu_mean_stats_.accel_mean *= inv_n;
    imu_mean_stats_.gyro_mean *= inv_n;
}


void StaticInitializer::UpdateAlignmentCache() {
    alignment_cache_ = AlignmentCache();
    if (imu_mean_stats_.sample_count == 0) {
        return;
    }

    alignment_cache_.accel_norm = imu_mean_stats_.accel_mean.norm();
    alignment_cache_.gravity_norm = origin_gravity_enu_.norm();
    if (alignment_cache_.accel_norm <= 1e-9 || alignment_cache_.gravity_norm <= 1e-9) {
        return;
    }

    alignment_cache_.accel_dir = imu_mean_stats_.accel_mean / alignment_cache_.accel_norm;
    alignment_cache_.gravity_dir = origin_gravity_enu_ / alignment_cache_.gravity_norm;
    alignment_cache_.accel_dir_aligned = accel_points_with_gravity_
                                             ? alignment_cache_.accel_dir
                                             : -alignment_cache_.accel_dir;
    alignment_cache_.valid = true;
}


StaticInitializer::ImuBiasResult StaticInitializer::ComputeImuBias() const {
    ImuBiasResult result;
    if (imu_mean_stats_.sample_count == 0 || !alignment_cache_.valid) {
        return result;
    }

    result.gyro_bias = imu_mean_stats_.gyro_mean;

    // Compute accel bias using the precise rotation matrix R_wb from gravity alignment.
    // The expected accelerometer reading in body frame when static is:
    //   expected_accel = R_wb^T * gravity_enu  (gravity_enu points upward ≈ [0, 0, +g])
    // Accel bias = measured mean - expected
    auto alignment = AlignImuWithGravity();
    const Eigen::Vector3d expected_accel_body = alignment.R_enu_from_imu.transpose() * origin_gravity_enu_;
    result.accel_bias = imu_mean_stats_.accel_mean - expected_accel_body;

    return result;
}


StaticInitializer::ImuAlignmentResult StaticInitializer::AlignImuWithGravity() const {
    ImuAlignmentResult result;
    if (imu_mean_stats_.sample_count == 0 || !alignment_cache_.valid) {
        return result;
    }

    const Eigen::Quaterniond q_align =
            Eigen::Quaterniond::FromTwoVectors(alignment_cache_.accel_dir_aligned, alignment_cache_.gravity_dir);
    // gravity_dir already follows the project "upward gravity" convention.
    // Yaw must rotate around the world Up axis to keep ENU yaw sign consistent.
    const Eigen::Vector3d up_dir = alignment_cache_.gravity_dir;
    const Eigen::Quaterniond q_yaw(Eigen::AngleAxisd(0.0, up_dir));

    const Eigen::Matrix3d R = (q_yaw * q_align).toRotationMatrix();
    result.R_enu_from_imu = R;

    result.roll = std::atan2(R(2, 1), R(2, 2));
    const double sin_pitch = -R(2, 0);
    const double sin_pitch_clamped = std::max(-1.0, std::min(1.0, sin_pitch));
    result.pitch = std::asin(sin_pitch_clamped);

    return result;
}
