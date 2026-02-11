#ifndef INITIALIZATION_MONITOR_H
#define INITIALIZATION_MONITOR_H

#include <atomic>
#include <deque>
#include <mutex>
#include <INIReader.h>
#include <Eigen/Core>

#include "data_type.h"
#include "orientation_initializer.h"


/// Monitors incoming IMU and GNSS data to determine when static initialization is complete.
///
/// Flow:
///   1. Accumulate IMU samples in a sliding window (default 10s at 100Hz).
///   2. When the window passes all stationary thresholds, record stationary start time.
///   3. After min_stationary_duration_s of continuous stationarity, compute InitializationResult
///      using all IMU data from stationary start.
///   4. Recompute every recompute_interval_s with the growing window.
///   5. After required_stable_count consecutive computations where roll/pitch delta < threshold,
///      AND GNSS is RTK_FIXED with position std below threshold, declare initialization complete.
class InitializationMonitor {
public:
    struct Config {
        // Stationary detection thresholds
        double accel_gravity_threshold;
        double accel_var_threshold;
        double gyro_var_threshold;
        double gyro_mean_threshold_xy;
        double gyro_mean_threshold_z;
        int imu_freq;

        // Initialization parameters
        int min_stationary_duration_s; // Minimum seconds of stationarity before first computation
        int recompute_interval_s; // Recompute interval in seconds
        int required_stable_count; // Consecutive stable computations needed
        double stability_threshold_deg; // Max roll/pitch delta (degrees) for stability

        // GNSS conditions
        double gnss_position_std_threshold; // meters

        // Gravity
        double gravity; // m/s^2
    };

    explicit InitializationMonitor(const INIReader &configures);

    ~InitializationMonitor() = default;

    /// Called from receiver's IMU callback (100 Hz). Thread-safe.
    void OnImuData(const RawIMUData &raw_imu);

    /// Called from receiver's GNSS callback (1 Hz). Thread-safe.
    void OnGnssData(const GNSSSolutionData &gnss);

    /// Check if static initialization has been declared complete.
    bool IsInitialized() const { return initialized_.load(std::memory_order_acquire); }

    /// Retrieve the final initialization result. Only valid after IsInitialized() returns true.
    InitializationResult GetResult() const;

private:
    Config config_{};
    std::atomic<bool> initialized_{false};

    // --- Sliding window stationary detection (O(1) per sample) ---
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

    size_t window_samples_{}; // Number of samples in the detection window
    std::deque<ImuData> detection_window_; // Sliding window for stationary detection
    WindowStats window_stats_; // Running statistics for the detection window

    // --- Stationary state ---
    bool is_stationary_ = false;
    double stationary_start_time_ = 0.0; // GPS time when stationarity was first detected

    // --- Growing computation buffer (from stationary start) ---
    std::deque<ImuData> computation_buffer_; // All IMU since stationary start

    // --- Computation scheduling ---
    double last_computation_time_ = 0.0; // GPS time of last computation

    // --- Stability tracking ---
    int stable_count_ = 0; // Consecutive stable computations
    InitializationResult last_result_; // Previous computation result
    bool has_previous_result_ = false;

    // --- Final result ---
    InitializationResult final_result_;

    // --- GNSS state ---
    GNSSSolutionData latest_gnss_{};
    bool has_gnss_ = false;

    mutable std::mutex mutex_;

    // --- Loading config ---
    void LoadConfig(const INIReader &configures);

    // --- Internal methods ---
    bool IsStaticWindow() const;

    void ComputeAndCheck(double current_time);

    bool CheckStability(const InitializationResult &new_result);

    bool CheckGnssConditions() const;

    void Reset();
};


#endif // INITIALIZATION_MONITOR_H
