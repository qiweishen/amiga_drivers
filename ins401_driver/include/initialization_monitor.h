#ifndef INITIALIZATION_MONITOR_H
#define INITIALIZATION_MONITOR_H

#include <Eigen/Core>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "ins401_data_type.h"
#include "orientation_initializer.h"


// Monitors incoming IMU and GNSS data to determine when static initialization is complete.
//
// Flow:
//   1. Accumulate IMU samples in a sliding window (default 10s at 100Hz).
//   2. When the window passes all stationary thresholds, record stationary start time.
//   3. After min_stationary_duration_s of continuous stationarity, compute InitializationResult
//      using all IMU data from stationary start.
//   4. Recompute every recompute_interval_s with the growing window.
//   5. After required_stable_count consecutive computations where roll/pitch delta < threshold,
//      AND GNSS is RTK_FIXED with position std below threshold, declare initialization complete.
class InitializationMonitor {
public:
	struct Options {
		// GNSS conditions
		bool enable_gnss_check;
		double gnss_horizontal_std_threshold;  // m

		// IMU Stationary detection thresholds
		int imu_freq = 100;				// Hz
		double accel_gravity_threshold;
		double accel_var_threshold;		// m/s^2
		double gyro_var_threshold;		// deg/s
		double gyro_mean_threshold_xy;	// deg/s
		double gyro_mean_threshold_z;	// deg/s

		// IMU initialization parameters
		double min_stationary_duration_s;  // Minimum seconds of stationarity before first computation
		double recompute_interval_s;	   // Recompute interval in seconds
		int required_stable_count;		   // Consecutive stable computations needed
		double stability_threshold_deg;	   // Max roll/pitch delta (degrees) for stability

		// Gravity
		double local_gravity;  // m/s^2
	};

	explicit InitializationMonitor(const INSConfig &config);

	~InitializationMonitor() = default;

	// Called from receiver's IMU callback (100 Hz). Thread-safe.
	void OnImuData(const RawIMUData &raw_imu);

	// Called from receiver's NMEA callback. Thread-safe.
	void OnGnssData(const GNSSSolutionData &gnss);

	void WaitForFirstGnssAndGravity(std::chrono::milliseconds timeout);

	void SetBlhFromGga(const Eigen::Vector3d &blh);

	[[nodiscard]] bool IsInitialized() const { return initialized_.load(std::memory_order_acquire); }

	[[nodiscard]] InitializationResult GetResult() const;

private:
	Options config_{};
	std::atomic<bool> initialized_{ false };

	size_t window_samples_{};
	std::deque<ImuData> detection_window_;
	ImuWindowStats window_stats_;

	// --- Stationary state ---
	bool is_stationary_ = false;
	double stationary_start_time_ = 0.0;  // GPS time when stationarity was first detected

	// --- Growing computation buffer (from stationary start) ---
	std::deque<ImuData> computation_buffer_;  // All IMU since stationary start

	// --- Computation scheduling ---
	double last_computation_time_ = 0.0;  // GPS time of last computation

	// --- Stability tracking ---
	int stable_count_ = 0;				// Consecutive stable computations
	InitializationResult last_result_;	// Previous computation result
	bool has_previous_result_ = false;

	// --- Final result ---
	InitializationResult final_result_;

	// --- GNSS state ---
	GNSSSolutionData latest_position_{};
	bool has_position_ = false;
	bool gravity_ready_ = false;

	mutable std::mutex mutex_;
	std::condition_variable gravity_cv_;

	// --- Internal methods ---
	bool IsStaticWindow() const;

	void ComputeAndCheck(double current_time);

	bool CompareResults(const InitializationResult &new_result) const;

	bool CheckGnssConditions() const;

	void Reset();
};


#endif	// INITIALIZATION_MONITOR_H
