#include "stationary_detector.h"

#include <spdlog/spdlog.h>

#include "utility.h"


namespace INS401 {
namespace {
	constexpr std::string_view kModule = "StationaryDetector";
}


StationaryDetector::StationaryDetector(const std::vector<RawIMUData> &raw_imu_data, double local_gravity, const Config &cfg) :
	accel_gravity_threshold_(cfg.accel_gravity_threshold),
	accel_var_threshold_(cfg.accel_var_threshold),
	gyro_var_threshold_(cfg.gyro_var_threshold),
	gyro_mean_threshold_xy_(cfg.gyro_mean_threshold_xy),
	gyro_mean_threshold_z_(cfg.gyro_mean_threshold_z),
	imu_freq_(cfg.imu_freq),
	min_duration_s_(cfg.min_duration_s),
	window_samples_(static_cast<size_t>(min_duration_s_ * imu_freq_)),
	local_gravity_(local_gravity < 0.0 ? -local_gravity : local_gravity) {
	imu_data_.reserve(raw_imu_data.size());
	for (const auto &raw: raw_imu_data) {
		imu_data_.push_back(ToImuData(raw));
	}
	FindStationaryTimeSegments();
	FindStationaryImuSegments();
}


ImuWindowStats StationaryDetector::InitializeWindowStats() const {
	ImuWindowStats stats;
	for (size_t i = 0; i < window_samples_; ++i) {
		stats.Add(imu_data_[i]);
	}
	return stats;
}


bool StationaryDetector::IsStaticWindow(const ImuWindowStats &stats) const {
	const double inv_n = 1.0 / static_cast<double>(window_samples_);
	const Eigen::Vector3d accel_mean = stats.accel_sum * inv_n;
	const Eigen::Vector3d gyro_mean = stats.gyro_sum * inv_n;

	// Variance = E[X^2] - E[X]^2, clamped to zero for numerical safety
	const double accel_std = std::sqrt(std::max(0.0, stats.accel_sq_sum * inv_n - accel_mean.squaredNorm()));
	const double gyro_std = std::sqrt(std::max(0.0, stats.gyro_sq_sum * inv_n - gyro_mean.squaredNorm()));

	return ((std::abs(gyro_mean.x()) < gyro_mean_threshold_xy_) && (std::abs(gyro_mean.y()) < gyro_mean_threshold_xy_) &&
			(std::abs(gyro_mean.z()) < gyro_mean_threshold_z_)) &&
		   (gyro_std < gyro_var_threshold_) && (std::abs(accel_mean.norm() - local_gravity_) < accel_gravity_threshold_) &&
		   (accel_std < accel_var_threshold_);
}


void StationaryDetector::SlideWindowStats(size_t window_start, size_t window_end, size_t step, ImuWindowStats &stats) const {
	for (size_t i = 0; i < step; ++i) {
		stats.Remove(imu_data_[window_start + i]);
	}
	for (size_t i = 0; i < step; ++i) {
		stats.Add(imu_data_[window_end + 1 + i]);
	}
}


void StationaryDetector::AppendSegmentIfValid(size_t seg_start_idx, size_t seg_end_idx) {
	if (seg_end_idx < seg_start_idx) {
		return;
	}

	// Decision indices point to the window *end*; subtract window size to recover
	// the actual data start covered by the first passing window.
	const size_t adjusted_start = (seg_start_idx + 1 >= window_samples_) ? (seg_start_idx + 1 - window_samples_) : 0;
	const size_t adjusted_end = std::min(seg_end_idx, imu_data_.size() - 1);

	const double start_time = GpsWeekTowToSec(imu_data_[adjusted_start].gps_week, imu_data_[adjusted_start].gps_millisecs);
	const double end_time = GpsWeekTowToSec(imu_data_[adjusted_end].gps_week, imu_data_[adjusted_end].gps_millisecs);

	if (end_time < start_time) {
		return;
	}
	if (end_time - start_time < static_cast<double>(min_duration_s_)) {
		return;
	}

	// Merge with previous segment if overlapping
	if (!stationary_time_segments_.empty() && start_time <= stationary_time_segments_.back().second) {
		stationary_time_segments_.back().second = std::max(stationary_time_segments_.back().second, end_time);
	} else {
		stationary_time_segments_.emplace_back(start_time, end_time);
	}
}


void StationaryDetector::FindStationaryTimeSegments() {
	stationary_time_segments_.clear();

	if (imu_data_.empty()) {
		Common::Log::log_and_throw(kModule, "IMU data is empty.");
		return;
	}
	if (imu_data_.size() < window_samples_) {
		Common::Log::log_and_throw(
				kModule, fmt::format("Not enough IMU samples ({}) for window size ({}).", imu_data_.size(), window_samples_));
		return;
	}

	ImuWindowStats stats = InitializeWindowStats();

	bool in_segment = false;
	size_t seg_start_idx = 0;
	size_t seg_end_idx = 0;

	size_t window_start = 0;
	auto window_end = window_samples_ - 1;

	while (true) {
		if (IsStaticWindow(stats)) {
			if (!in_segment) {
				in_segment = true;
				seg_start_idx = window_end;
			}
			seg_end_idx = window_end;
		} else if (in_segment) {
			AppendSegmentIfValid(seg_start_idx, seg_end_idx);
			in_segment = false;
		}

		if (window_end + step_samples_ >= imu_data_.size())
			break;

		SlideWindowStats(window_start, window_end, step_samples_, stats);
		window_start += step_samples_;
		window_end += step_samples_;
	}

	if (in_segment) {
		AppendSegmentIfValid(seg_start_idx, seg_end_idx);
	}
}


void StationaryDetector::FindStationaryImuSegments() {
	stationary_imu_segments_.clear();
	stationary_imu_segments_.reserve(stationary_time_segments_.size());

	for (const auto &[t_start, t_end]: stationary_time_segments_) {
		// Binary search for the first sample >= t_start
		auto it_begin = std::lower_bound(imu_data_.begin(), imu_data_.end(), t_start,
										 [](const ImuData &d, double t) { return GpsWeekTowToSec(d.gps_week, d.gps_millisecs) < t; });

		// Binary search for the first sample > t_end
		auto it_end = std::upper_bound(it_begin, imu_data_.end(), t_end,
									   [](double t, const ImuData &d) { return t < GpsWeekTowToSec(d.gps_week, d.gps_millisecs); });

		if (it_begin != it_end) {
			stationary_imu_segments_.emplace_back(it_begin, it_end);
		}
	}
}
}  // namespace INS401
