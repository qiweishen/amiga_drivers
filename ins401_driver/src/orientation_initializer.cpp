#include "orientation_initializer.h"

#include <eigen3/Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <limits>


StaticInitializer::StaticInitializer(const double &gravity, const std::vector<RawIMUData> &static_imu, double yaw,
                                     double yaw_std, double accel_noise_std,
                                     bool accel_points_with_gravity)
    : origin_gravity_enu_(0.0, 0.0, -std::abs(gravity)),
      yaw_(yaw),
      yaw_std_(yaw_std),
      accel_noise_std_(accel_noise_std),
      accel_points_with_gravity_(accel_points_with_gravity) {
    // Convert RawIMUData to ImuData
    static_imu_.reserve(static_imu.size());
    for (const auto &raw : static_imu) {
        static_imu_.push_back(ToImuData(raw));
    }
    UpdateImuMeanStats();
    UpdateAlignmentCache();
}

StaticInitializer::StaticInitializer(const Eigen::Vector3d &gravity_enu, const std::vector<ImuData> &static_imu,
                                     double yaw, double yaw_std, double accel_noise_std,
                                     bool accel_points_with_gravity)
    : origin_gravity_enu_(gravity_enu),
      static_imu_(static_imu),
      yaw_(yaw),
      yaw_std_(yaw_std),
      accel_noise_std_(accel_noise_std),
      accel_points_with_gravity_(accel_points_with_gravity) {
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

    const bool yaw_valid = std::isfinite(yaw_) && yaw_ >= -M_PI && yaw_ <= M_PI;
    const double used_yaw = yaw_valid ? yaw_ : 0.0;
    result.yaw = used_yaw;
    result.yaw_std = yaw_valid ? yaw_std_ : 0.0;

    const Eigen::Quaterniond q_align =
            Eigen::Quaterniond::FromTwoVectors(alignment_cache_.accel_dir_aligned, alignment_cache_.gravity_dir);
    // gravity_dir already follows the project "upward gravity" convention.
    // Yaw must rotate around the world Up axis to keep ENU yaw sign consistent.
    const Eigen::Vector3d up_dir = alignment_cache_.gravity_dir;
    const Eigen::Quaterniond q_yaw(Eigen::AngleAxisd(used_yaw, up_dir));

    const Eigen::Matrix3d R = (q_yaw * q_align).toRotationMatrix();
    result.R_enu_from_imu = R;

    result.roll = std::atan2(R(2, 1), R(2, 2));
    const double sin_pitch = -R(2, 0);
    const double sin_pitch_clamped = std::max(-1.0, std::min(1.0, sin_pitch));
    result.pitch = std::asin(sin_pitch_clamped);

    if (accel_noise_std_ > 0.0) {
        const double cos_pitch = std::cos(result.pitch);
        if (std::abs(cos_pitch) > std::numeric_limits<double>::epsilon()) {
            result.roll_std = accel_noise_std_ / (alignment_cache_.gravity_norm * cos_pitch);
        }
        result.pitch_std = accel_noise_std_ / alignment_cache_.gravity_norm;
    }

    return result;
}


// --- YawEstimator ---

YawEstimator::YawEstimator(const std::vector<GnssData> &gnss_data,
                           double min_speed,
                           int max_epochs)
    : gnss_data_(gnss_data),
      min_speed_(min_speed),
      max_epochs_(max_epochs) {
}


YawEstimator::YawResult YawEstimator::EstimateYaw() const {
    YawResult result;

    if (gnss_data_.empty()) {
        return result; // invalid, defaults to yaw=0, large std
    }

    // Collect heading samples from GNSS epochs with sufficient horizontal speed
    std::vector<double> yaw_samples;
    std::vector<double> weights;
    yaw_samples.reserve(max_epochs_);
    weights.reserve(max_epochs_);

    for (const auto &gnss: gnss_data_) {
        const double v_east = gnss.enu_vel(0);
        const double v_north = gnss.enu_vel(1);
        const double h_speed = std::sqrt(v_east * v_east + v_north * v_north);

        if (h_speed < min_speed_) {
            continue;
        }

        // yaw = atan2(east, north) in ENU convention
        const double yaw = std::atan2(v_east, v_north);
        yaw_samples.push_back(yaw);

        // Weight by speed (faster = more reliable heading)
        weights.push_back(h_speed);

        if (static_cast<int>(yaw_samples.size()) >= max_epochs_) {
            break;
        }
    }

    if (yaw_samples.empty()) {
        // No valid GNSS epochs with sufficient speed
        return result; // invalid
    }

    // Weighted circular mean to handle angle wrapping
    double sin_sum = 0.0;
    double cos_sum = 0.0;
    double weight_sum = 0.0;

    for (size_t i = 0; i < yaw_samples.size(); ++i) {
        sin_sum += weights[i] * std::sin(yaw_samples[i]);
        cos_sum += weights[i] * std::cos(yaw_samples[i]);
        weight_sum += weights[i];
    }

    sin_sum /= weight_sum;
    cos_sum /= weight_sum;

    result.yaw = std::atan2(sin_sum, cos_sum);
    result.valid = true;

    // Estimate yaw uncertainty from circular variance
    const double R_len = std::sqrt(sin_sum * sin_sum + cos_sum * cos_sum);
    if (R_len > 1e-9 && R_len <= 1.0) {
        // Circular variance: V = 1 - R, circular std = sqrt(-2 * ln(R))
        result.yaw_std = std::sqrt(-2.0 * std::log(R_len));
    } else if (yaw_samples.size() == 1) {
        // Single sample: estimate from GNSS velocity std and speed
        const double h_speed = weights[0];
        const double vel_std = 0.03; // typical GNSS Doppler velocity std
        result.yaw_std = vel_std / std::max(h_speed, 0.1);
    } else {
        result.yaw_std = 0.1; // fallback: ~6 degrees
    }

    return result;
}