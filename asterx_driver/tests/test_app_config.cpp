// SPDX-License-Identifier: BSD-3-Clause
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "app_config.hpp"

namespace {

std::filesystem::path write_temp_config(const std::string& yaml) {
    static int seq = 0;
    const auto path = std::filesystem::temp_directory_path() /
                      ("asterx_app_config_test_" + std::to_string(++seq) + ".yaml");
    std::ofstream out(path);
    out << yaml;
    out.close();
    return path;
}

}  // namespace

TEST(AppConfig, MissingLeverArmFails) {
    const auto path = write_temp_config(R"yaml(
receiver:
  imu:
    startup_data_mode: Boot
    orientation_mode: SensorDefault
)yaml");

    EXPECT_THROW((void) asterx::load_app_config(path.string()),
                 asterx::ConfigLoadError);
    std::filesystem::remove(path);
}

TEST(AppConfig, LoadsManualOrientationConfig) {
    const auto path = write_temp_config(R"yaml(
connection:
  host: "192.0.2.10"
  user: "admin"
  password: "secret"
receiver:
  imu:
    startup_data_mode: Boot
    orientation_mode: manual
    theta_x_deg: 1.0
    theta_y_deg: 2.0
    theta_z_deg: 3.0
    ant_lever_arm_m:
      x: 0.1
      y: -0.2
      z: 0.3
  gnss_attitude:
    mode: MultiAntenna
    attitude_offset_deg:
      heading: 4.0
      pitch: -5.0
  tracking:
    enable_all: false
    cn0_mask_dbhz: 12
)yaml");

    const auto cfg = asterx::load_app_config(path.string());
    EXPECT_EQ(cfg.host, "192.0.2.10");
    EXPECT_EQ(cfg.receiver.imu_orientation_mode, "manual");
    EXPECT_DOUBLE_EQ(cfg.receiver.theta_x_deg, 1.0);
    EXPECT_DOUBLE_EQ(cfg.receiver.ant_lever_arm_m.y, -0.2);
    EXPECT_DOUBLE_EQ(cfg.receiver.attitude_offset_deg.pitch_deg, -5.0);
    EXPECT_FALSE(cfg.receiver.configure_all_tracking);
    EXPECT_EQ(cfg.receiver.cn0_mask_dbhz, 12);
    EXPECT_EQ(cfg.receiver.streams.front().interval, "OnChange");
    std::filesystem::remove(path);
}

TEST(AppConfig, RejectsRotateBytesAtOrAbove2GiB) {
    const auto path = write_temp_config(R"yaml(
output:
  rotate_bytes: 2147483648
receiver:
  imu:
    startup_data_mode: Boot
    orientation_mode: SensorDefault
    ant_lever_arm_m:
      x: 0.0
      y: 0.0
      z: 0.0
)yaml");

    EXPECT_THROW((void) asterx::load_app_config(path.string()),
                 asterx::ConfigLoadError);
    std::filesystem::remove(path);
}

TEST(AppConfig, InvalidIntervalFails) {
    const auto path = write_temp_config(R"yaml(
receiver:
  imu:
    startup_data_mode: Boot
    orientation_mode: SensorDefault
    ant_lever_arm_m:
      x: 0.0
      y: 0.0
      z: 0.0
  streams:
    - id: 1
      blocks: [ "ExtSensorMeas" ]
      interval: "sec3"
)yaml");

    EXPECT_THROW((void) asterx::load_app_config(path.string()),
                 asterx::ConfigLoadError);
    std::filesystem::remove(path);
}
