#ifndef STATIONARY_DETECTOR_H
#define STATIONARY_DETECTOR_H

#include <utility>
#include <vector>
#include <Eigen/Core>

#include "data_type.h"
#include "tool.h"


class StationaryDetector {
public:
    struct Config {
        double accel_gravity_threshold;
        double accel_var_threshold;
        double gyro_var_threshold;
        double gyro_mean_threshold_xy;
        double gyro_mean_threshold_z;
        int imu_freq;
        int min_duration_s;
    };

    explicit StationaryDetector(const std::vector<RawIMUData> &raw_imu_data, double local_gravity,
                                const Config &cfg = Config{});

    ~StationaryDetector() = default;

    std::vector<std::pair<double, double> > GetStationaryTimeSegments() {
        return stationary_time_segments_;
    }

    std::vector<std::vector<ImuData> > &GetStationaryImuSegments() {
        return stationary_imu_segments_;
    }

private:
    // --- Configuration ---
    double accel_gravity_threshold_;
    double accel_var_threshold_;
    double gyro_var_threshold_;
    double gyro_mean_threshold_xy_;
    double gyro_mean_threshold_z_;
    int imu_freq_;
    int min_duration_s_;
    size_t window_samples_;
    size_t step_samples_ = 1;

    // --- Data ---
    std::vector<ImuData> imu_data_;
    double local_gravity_;
    std::vector<std::pair<double, double> > stationary_time_segments_;
    std::vector<std::vector<ImuData> > stationary_imu_segments_;

    // --- Internal helpers ---
    struct WindowStats {
        Eigen::Vector3d gyro_sum = Eigen::Vector3d::Zero();
        Eigen::Vector3d accel_sum = Eigen::Vector3d::Zero();
        double gyro_sq_sum = 0.0;
        double accel_sq_sum = 0.0;

        void Add(const ImuData &sample) {
            gyro_sum += sample.gyro;
            accel_sum += sample.accel;
            gyro_sq_sum += sample.gyro.squaredNorm();
            accel_sq_sum += sample.accel.squaredNorm();
        }

        void Remove(const ImuData &sample) {
            gyro_sum -= sample.gyro;
            accel_sum -= sample.accel;
            gyro_sq_sum -= sample.gyro.squaredNorm();
            accel_sq_sum -= sample.accel.squaredNorm();
        }
    };

    // Initialize window statistics over the first window_samples_ samples.
    WindowStats InitializeWindowStats() const;

    // Check if the current window is stationary based on gyro/accel statistics.
    bool IsStaticWindow(const WindowStats &stats) const;

    // Slide the window forward by `step` samples.
    void SlideWindowStats(size_t window_start, size_t window_end, size_t step, WindowStats &stats) const;

    // Append a validated stationary segment, merging overlapping segments.
    void AppendSegmentIfValid(size_t seg_start_idx, size_t seg_end_idx);

    // Main detection loop: slide a fixed-size window across IMU data.
    void FindStationaryTimeSegments();

    // Build IMU data segments from detected time ranges using binary search.
    void FindStationaryImuSegments();
};

#endif // STATIONARY_DETECTOR_H