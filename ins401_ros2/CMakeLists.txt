cmake_minimum_required(VERSION 3.22...3.25)
project(ins401_ros2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(std_msgs REQUIRED)

# Include directories
include_directories(
  ins401_driver/include
)

# INS401 Driver library
add_library(ins401_driver STATIC
  ins401_driver/src/ins_driver.cpp
  ins401_driver/src/tool.cpp
  ins401_driver/src/ins_device_finder.cpp
)

# IMU Publisher Node
add_executable(imu_publisher
  src/imu_publisher.cpp
)

# Link the ins401_driver library
target_link_libraries(imu_publisher
  ins401_driver
)

# ROS2 dependencies
ament_target_dependencies(imu_publisher
  rclcpp
  sensor_msgs
  geometry_msgs
  std_msgs
)

# Install executables
install(TARGETS
  imu_publisher
  DESTINATION lib/${PROJECT_NAME}
)

# Install launch files
install(DIRECTORY
  launch
  DESTINATION share/${PROJECT_NAME}/
)

# Install config files
install(DIRECTORY
  config
  DESTINATION share/${PROJECT_NAME}/
)

ament_package()
