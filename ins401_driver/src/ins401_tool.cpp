#include "ins401_tool.h"

#include "string_util.h"

#include <filesystem>
#include <spdlog/spdlog.h>

#include "utility.h"


namespace INS401::Tool {
	namespace Earth {
		constexpr std::string_view kModule = "INS401Tool::Earth";

		// Here we use the WGS84 ellipsoid model to compute gravity based on latitude, longitude, and height.
		// To distinguish from other LLH (deg, deg, m) formats, we use BLH (rad, rad, m) for the input.
		// BLH: Breite (Latitude, radians), Länge (Longitude, radians), Höhe (Height, meters).
		// Returned gravity is **POSITIVE** in m/s^2.
		double ComputeGravity(const Eigen::Vector3d &blh) {
			// Check for valid latitude and longitude ranges
			// Although longitude can be any value since it doesn't affect gravity, we still check it
			if (blh[0] < -M_PI / 2 || blh[0] > M_PI / 2 || blh[1] < -M_PI || blh[1] > M_PI) {
				Common::Log::log_and_throw(kModule, "Latitude / Longitude must be in radians and within valid ranges");
			}
			// Following formula adapted from: https://github.com/i2Nav-WHU/KF-GINS/blob/main/src/common/earth.h
			double sinphi = sin(blh[0]);
			double sin2 = sinphi * sinphi;
			double sin4 = sin2 * sin2;
			// Normal gravity at equator
			double gamma_a = 9.7803267715;
			// Series expansion of normal gravity at given latitude
			double gamma_0 = gamma_a *
							 (1 + 0.0052790414 * sin2 + 0.0000232718 * sin4 + 0.0000001262 * sin2 * sin4 + 0.0000000007 * sin4 * sin4);
			// Changes of normal gravity with height
			double gamma = gamma_0 - (3.0877e-6 - 4.3e-9 * sin2) * blh[2] + 0.72e-12 * blh[2] * blh[2];
			return gamma;
		}
	}  // namespace Earth
	namespace Utility {
		constexpr std::string_view kModule = "INS401Tool::Utility";

		std::vector<std::string> SplitString(std::string_view str, char delimiter) {
			return Common::StringUtil::Split(str, delimiter);
		}
	}  // namespace Utility


	void LoadConfig(std::string_view config_path, INSConfig &config) {
		// Delegate YAML I/O to the common ConfigLoader (throws on error)
		Common::ConfigLoader loader(config_path);
		const auto &root = loader.root();

		// NTRIP Client
		const auto &ntrip = root["NTRIP Client"];
		config.enable_rtk = ntrip["Enable RTK"].as<bool>(false);
		config.host = ntrip["Host"].as<std::string>("localhost");
		config.port = ntrip["Port"].as<int>(2101);
		config.mount_point = ntrip["Mount Point"].as<std::string>("MOUNT");
		config.use_vrs = ntrip["Use VRS"].as<bool>(false);
		config.username = ntrip["Username"].as<std::string>("user");
		config.password = ntrip["Password"].as<std::string>("password");

		// Static Initialization
		const auto &init = root["Static Initialization"];
		config.enable_gnss_checking = init["Enable GNSS Checking"].as<bool>(false);
		config.gnss_horizontal_std_threshold = init["GNSS Horizontal STD"].as<double>(0.03);
		config.accel_gravity_threshold = init["Accel Gravity Threshold"].as<double>(0.035);
		config.accel_var_threshold = init["Accel Variance Threshold"].as<double>(0.008);
		config.gyro_var_threshold = init["Gyro Variance Threshold"].as<double>(0.125);
		config.gyro_mean_threshold_xy = init["Gyro Mean Threshold_xy"].as<double>(0.035);
		config.gyro_mean_threshold_z = init["Gyro Mean Threshold_z"].as<double>(0.125);
		config.min_stationary_duration_s = init["Minimal Stationary Duration"].as<double>(10.0);
		config.recompute_interval_s = init["Recompute Interval"].as<double>(5.0);
		config.required_stable_count = init["Required Stable Count"].as<int>(5);
		config.stability_threshold_deg = init["Stability Initialization Threshold deg"].as<double>(0.1);

		// Logging (optional, defaults to true)
		if (root["Logging System"]) {
			config.enable_logging = root["Logging System"]["Enable Logging"].as<bool>(true);
		}
	}
}  // namespace INS401::Tool
