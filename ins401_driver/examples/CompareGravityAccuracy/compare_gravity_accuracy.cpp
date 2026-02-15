#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <GeographicLib/NormalGravity.hpp>
#include <GeographicLib/LocalCartesian.hpp>

#include "Eigen/Core"
#include "tool.h"


double ComputeReferenceGravity(Eigen::Vector3d &llh) {
    GeographicLib::Geocentric geocentric_model(GeographicLib::Geocentric::WGS84());
    GeographicLib::NormalGravity gravity_model(GeographicLib::NormalGravity::WGS84());

    double x, y, z;
    std::vector<double> M(9);
    geocentric_model.Forward(llh(0), llh(1), llh(2), x, y, z, M);
    double gx, gy, gz;
    gravity_model.U(x, y, z, gx, gy, gz);

    // M maps local ENU -> ECEF (row-major). Use transpose for ECEF -> ENU.
    const Eigen::Matrix3d R_local_to_ecef = Eigen::Map<const Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(M.data());
    Eigen::Vector3d g_ecef(gx, gy, gz);

    Eigen::Vector3d g_enu_raw = R_local_to_ecef.transpose() * g_ecef;
    Eigen::Vector3d gravity_enu;
    // Ensure the gravity vector points upward (positive Up / Z component in ENU).
    // GeographicLib's U() gradient points toward Earth center (downward, negative Z in ENU).
    if (g_enu_raw(2) < 0.0) {
        gravity_enu = -g_enu_raw;
    } else {
        gravity_enu = g_enu_raw;
    }
    return gravity_enu.norm();
}


Eigen::Vector3d GetRandomGeodeticPosition() {
    static std::mt19937 gen(std::random_device{}());
    // Latitude: -90 to 90, Longitude: -180 to 180, Height: 0 to 250 meters.
    std::uniform_real_distribution<double> lat_dist(-90.0, 90.0);
    std::uniform_real_distribution<double> lon_dist(-180.0, 180.0);
    std::uniform_real_distribution<double> hgt_dist(0.0, 250.0);
    return {lat_dist(gen), lon_dist(gen), hgt_dist(gen)};
}


int main(int argc, char *argv[]) {
    constexpr int kNumTrials = 100;

    double max_error = std::numeric_limits<double>::lowest();
    double min_error = std::numeric_limits<double>::max();
    double sum_error = 0.0;
    double sum_error_sq = 0.0;

    Eigen::Vector3d max_error_pos, min_error_pos;

    for (int i = 0; i < kNumTrials; ++i) {
        Eigen::Vector3d pos_llh = GetRandomGeodeticPosition();
        // LLH (degrees) to BLH (radians)
        Eigen::Vector3d pos_blh(pos_llh(0) * M_PI / 180.0, pos_llh(1) * M_PI / 180.0, pos_llh(2));
        double reference = ComputeReferenceGravity(pos_llh);

        double computed = Tool::Earth::ComputeGravity(pos_blh);
        double error = std::abs(computed - reference);

        sum_error += error;
        sum_error_sq += error * error;

        if (error > max_error) {
            max_error = error;
            max_error_pos = pos_llh;
        }
        if (error < min_error) {
            min_error = error;
            min_error_pos = pos_llh;
        }
    }

    double mean_error = sum_error / kNumTrials;
    double variance = sum_error_sq / kNumTrials - mean_error * mean_error;

    std::cout << "========================================" << std::endl;
    std::cout << "  Gravity Accuracy Comparison Report" << std::endl;
    std::cout << "  Trials: " << kNumTrials << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Max Absolute Error:  " << max_error << " m/s^2" << std::endl;
    std::cout << "    at LLH: " << max_error_pos.transpose() << std::endl;
    std::cout << "  Min Absolute Error:  " << min_error << " m/s^2" << std::endl;
    std::cout << "    at LLH: " << min_error_pos.transpose() << std::endl;
    std::cout << "  Mean Absolute Error: " << mean_error << " m/s^2" << std::endl;
    std::cout << "  Variance:            " << variance << " (m/s^2)^2" << std::endl;
    std::cout << "  Std Deviation:       " << std::sqrt(variance) << " m/s^2" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
