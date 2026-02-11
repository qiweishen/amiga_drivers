#include "initialization_monitor.h"

#include <cmath>

#include "tool.h"


namespace {
    constexpr std::string_view kModule = "InitializationMonitor";
    constexpr double kDegToRad = M_PI / 180.0;
}


InitializationMonitor::InitializationMonitor(const INIReader &configures) {
    LoadConfig(configures);
    window_samples_ = config_.min_stationary_duration_s * config_.imu_freq;
    Tool::LogMessage(spdlog::level::trace, kModule,
                     fmt::format("Parameters: window={}s ({}samples), recompute={}s, "
                                 "stable_count={}, stability_threshold={:.2f}deg, "
                                 "gnss_std_threshold={:.4f}m, gravity={:.5f}m/s^2",
                                 config_.min_stationary_duration_s, window_samples_,
                                 config_.recompute_interval_s, config_.required_stable_count,
                                 config_.stability_threshold_deg,
                                 config_.gnss_position_std_threshold, config_.gravity));
}


void InitializationMonitor::OnImuData(const RawIMUData &raw_imu) {
    if (initialized_.load(std::memory_order_acquire)) {
        return; // Already initialized, nothing to do
    }

    const ImuData imu = ToImuData(raw_imu);
    const double current_time = GpsWeekTowToSec(imu.gps_week, imu.gps_millisecs);

    std::lock_guard<std::mutex> lock(mutex_);

    // --- Update sliding detection window ---
    detection_window_.push_back(imu);
    window_stats_.Add(imu);

    // Remove oldest sample(s) if window exceeds size
    while (detection_window_.size() > window_samples_) {
        window_stats_.Remove(detection_window_.front());
        detection_window_.pop_front();
    }

    // Not enough samples yet for detection
    if (detection_window_.size() < window_samples_) {
        return;
    }

    // --- Check stationarity ---
    if (IsStaticWindow()) {
        if (!is_stationary_) {
            // Just became stationary - record start time
            is_stationary_ = true;
            stationary_start_time_ = GpsWeekTowToSec(detection_window_.front().gps_week,
                                                     detection_window_.front().gps_millisecs);
            // Populate computation buffer with the full window
            computation_buffer_.clear();
            computation_buffer_.assign(detection_window_.begin(), detection_window_.end());

            Tool::LogMessage(spdlog::level::info, kModule,
                             fmt::format("Stationary detected at GPS time {:.3f}s", stationary_start_time_));
        } else {
            // Still stationary - append to growing computation buffer
            computation_buffer_.push_back(imu);
        }

        // Check if we have enough stationary data for computation
        const double stationary_duration = current_time - stationary_start_time_;
        if (stationary_duration >= static_cast<double>(config_.min_stationary_duration_s)) {
            // Check if it's time to (re)compute
            const bool first_computation = !has_previous_result_;
            const double time_since_last = current_time - last_computation_time_;
            if (first_computation || time_since_last >= static_cast<double>(config_.recompute_interval_s)) {
                ComputeAndCheck(current_time);
            }
        }
    } else {
        if (is_stationary_) {
            // Lost stationarity
            Tool::LogMessage(spdlog::level::warn, kModule,
                             fmt::format("Stationarity lost at GPS time {:.3f}s, resetting.", current_time));
            Reset();
        }
    }
}


void InitializationMonitor::OnGnssData(const GNSSSolutionData &gnss) {
    if (initialized_.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    latest_gnss_ = gnss;
    has_gnss_ = true;
}


InitializationResult InitializationMonitor::GetResult() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return final_result_;
}


void InitializationMonitor::LoadConfig(const INIReader &configures) {
    config_.accel_gravity_threshold = configures.
            GetReal("IMU Initial Initialization", "accel_gravity_threshold", 0.035);
    config_.accel_var_threshold = configures.GetReal("IMU Initial Initialization", "accel_var_threshold", 0.008);
    config_.gyro_var_threshold = configures.GetReal("IMU Initial Initialization", "gyro_var_threshold", 0.125);
    config_.gyro_mean_threshold_xy = configures.GetReal("IMU Initial Initialization", "gyro_mean_threshold_xy", 0.035);
    config_.gyro_mean_threshold_z = configures.GetReal("IMU Initial Initialization", "gyro_mean_threshold_z", 0.125);
    config_.imu_freq = static_cast<int>(configures.GetInteger("IMU Initial Initialization", "imu_freq", 100));
    config_.min_stationary_duration_s = static_cast<int>(configures.GetInteger(
        "IMU Initial Initialization", "min_stationary_duration_s", 10));
    config_.recompute_interval_s = static_cast<int>(configures.GetInteger(
        "IMU Initial Initialization", "recompute_interval_s", 5));
    config_.required_stable_count = static_cast<int>(configures.GetInteger(
        "IMU Initial Initialization", "required_stable_count", 5));
    config_.stability_threshold_deg = configures.GetReal("IMU Initial Initialization", "stability_threshold_deg", 0.1);
    config_.gnss_position_std_threshold = configures.GetReal("IMU Initial Initialization",
                                                             "gnss_position_std_threshold", 0.02);
    config_.gravity = configures.GetReal("IMU Initial Initialization", "gravity", 9.81);
}


bool InitializationMonitor::IsStaticWindow() const {
    const double inv_n = 1.0 / static_cast<double>(window_samples_);
    const Eigen::Vector3d accel_mean = window_stats_.accel_sum * inv_n;
    const Eigen::Vector3d gyro_mean = window_stats_.gyro_sum * inv_n;

    // Variance = E[X^2] - E[X]^2, clamped to zero for numerical safety
    const double accel_std = std::sqrt(std::max(0.0,
                                                window_stats_.accel_sq_sum * inv_n - accel_mean.squaredNorm()));
    const double gyro_std = std::sqrt(std::max(0.0,
                                               window_stats_.gyro_sq_sum * inv_n - gyro_mean.squaredNorm()));

    return (std::abs(gyro_mean.x()) < config_.gyro_mean_threshold_xy) &&
           (std::abs(gyro_mean.y()) < config_.gyro_mean_threshold_xy) &&
           (std::abs(gyro_mean.z()) < config_.gyro_mean_threshold_z) &&
           (gyro_std < config_.gyro_var_threshold) &&
           (std::abs(accel_mean.norm() - config_.gravity) < config_.accel_gravity_threshold) &&
           (accel_std < config_.accel_var_threshold);
}


void InitializationMonitor::ComputeAndCheck(double current_time) {
    last_computation_time_ = current_time;

    // Convert computation buffer to vector for StaticInitializer
    std::vector<ImuData> imu_segment(computation_buffer_.begin(), computation_buffer_.end());

    // Run StaticInitializer
    const Eigen::Vector3d gravity_enu(0.0, 0.0, -config_.gravity);
    StaticInitializer initializer(gravity_enu, imu_segment);

    auto alignment = initializer.AlignImuWithGravity();
    auto bias = initializer.ComputeImuBias();

    // Build InitializationResult
    InitializationResult result;
    result.timestamp = static_cast<std::uint32_t>(current_time);
    result.orientation = Eigen::Quaterniond(alignment.R_enu_from_imu);
    result.gyro_bias = bias.gyro_bias;
    result.accel_bias = bias.accel_bias;
    result.roll = alignment.roll;
    result.pitch = alignment.pitch;
    result.yaw = alignment.yaw;
    result.roll_std = alignment.roll_std;
    result.pitch_std = alignment.pitch_std;
    result.yaw_std = alignment.yaw_std;
    result.local_gravity = config_.gravity;

    // Fill position from latest GNSS if available
    if (has_gnss_) {
        result.position = Eigen::Vector3d(latest_gnss_.latitude, latest_gnss_.longitude, latest_gnss_.height);
    }

    const double stationary_duration = current_time - stationary_start_time_;
    Tool::LogMessage(spdlog::level::info, kModule,
                     fmt::format("Computation #{} at {:.1f}s stationary: roll={:.4f}deg pitch={:.4f}deg",
                                 stable_count_ + 1, stationary_duration,
                                 alignment.roll / kDegToRad, alignment.pitch / kDegToRad));

    // Check stability
    if (CheckStability(result)) {
        stable_count_++;
        Tool::LogMessage(spdlog::level::info, kModule,
                         fmt::format("Stable computation {}/{}", stable_count_, config_.required_stable_count));

        // Check if we have enough stable computations AND GNSS conditions are met
        if (stable_count_ >= config_.required_stable_count) {
            if (CheckGnssConditions()) {
                // Declaration: static initialization complete!
                final_result_ = result;
                initialized_.store(true, std::memory_order_release);

                Tool::LogMessage(spdlog::level::info, kModule,
                                 "=== STATIC INITIALIZATION COMPLETE ===");
                Tool::LogMessage(spdlog::level::info, kModule,
                                 fmt::format("  Roll:  {:.4f} deg (std: {:.4f} deg)",
                                             result.roll / kDegToRad, result.roll_std / kDegToRad));
                Tool::LogMessage(spdlog::level::info, kModule,
                                 fmt::format("  Pitch: {:.4f} deg (std: {:.4f} deg)",
                                             result.pitch / kDegToRad, result.pitch_std / kDegToRad));
                Tool::LogMessage(spdlog::level::info, kModule,
                                 fmt::format("  Yaw:   {:.4f} deg (std: {:.4f} deg)",
                                             result.yaw / kDegToRad, result.yaw_std / kDegToRad));
                Tool::LogMessage(spdlog::level::info, kModule,
                                 fmt::format("  Gyro bias:  [{:.6f}, {:.6f}, {:.6f}] deg/s",
                                             result.gyro_bias.x(), result.gyro_bias.y(), result.gyro_bias.z()));
                Tool::LogMessage(spdlog::level::info, kModule,
                                 fmt::format("  Accel bias: [{:.6f}, {:.6f}, {:.6f}] m/s^2",
                                             result.accel_bias.x(), result.accel_bias.y(), result.accel_bias.z()));
                Tool::LogMessage(spdlog::level::info, kModule,
                                 fmt::format("  GNSS position std: {:.4f} m",
                                             (latest_gnss_.latitude_std + latest_gnss_.longitude_std) / 2.0f));
            } else {
                Tool::LogMessage(spdlog::level::warn, kModule,
                                 "Stability reached but GNSS conditions not met. Waiting for RTK_FIXED and low std.");
            }
        }
    } else {
        if (has_previous_result_) {
            Tool::LogMessage(spdlog::level::warn, kModule,
                             fmt::format("Result unstable (roll delta={:.4f}deg, pitch delta={:.4f}deg). "
                                         "Reset stable count.",
                                         std::abs(result.roll - last_result_.roll) / kDegToRad,
                                         std::abs(result.pitch - last_result_.pitch) / kDegToRad));
        }
        stable_count_ = 0;
    }

    last_result_ = result;
    has_previous_result_ = true;
}


bool InitializationMonitor::CheckStability(const InitializationResult &new_result) {
    if (!has_previous_result_) {
        // First computation - cannot compare yet, treat as stable to start counting
        return true;
    }

    const double threshold_rad = config_.stability_threshold_deg * kDegToRad;
    const double roll_delta = std::abs(new_result.roll - last_result_.roll);
    const double pitch_delta = std::abs(new_result.pitch - last_result_.pitch);

    return (roll_delta < threshold_rad) && (pitch_delta < threshold_rad);
}


bool InitializationMonitor::CheckGnssConditions() const {
    if (!has_gnss_) {
        return false;
    }

    // RTK_FIXED = position_type 4
    if (latest_gnss_.position_type != 4) {
        return false;
    }

    // Position std check
    const double position_std = (static_cast<double>(latest_gnss_.latitude_std) +
                                 static_cast<double>(latest_gnss_.longitude_std)) / 2.0;
    return position_std < config_.gnss_position_std_threshold;
}


void InitializationMonitor::Reset() {
    is_stationary_ = false;
    stationary_start_time_ = 0.0;
    computation_buffer_.clear();
    last_computation_time_ = 0.0;
    stable_count_ = 0;
    has_previous_result_ = false;
    last_result_ = InitializationResult{};
}
