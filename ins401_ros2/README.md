# INS401 ROS2 Driver

ROS2 driver for INS401 IMU device. This package provides a node that captures IMU data from the INS401 device via Ethernet and publishes it as ROS2 messages.

## Architecture

The package consists of two main components:
1. **ins401_driver** - Low-level Ethernet driver that handles raw socket communication and packet parsing
2. **imu_publisher** - ROS2 node that uses the driver to get IMU data and publish it as ROS2 messages

## Features

- Captures IMU data (accelerometer and gyroscope) at 200Hz
- Publishes standard `sensor_msgs/msg/Imu` messages
- Optional data logging to file
- Configurable parameters via launch files or YAML config
- CRC validation for data integrity

## Prerequisites

- ROS2 Humble (or compatible version)
- Root/sudo privileges (required for raw socket access)
- INS401 device connected via Ethernet

## Building

```bash
# Navigate to your ROS2 workspace
cd ~/ros2_ws/src

# Clone or copy this package
cp -r /path/to/ins401_ros2 .

# Build the package
cd ~/ros2_ws
colcon build --packages-select ins401_ros2

# Source the workspace
source install/setup.bash
```

## Usage

### Basic Launch

Launch with default parameters:

```bash
sudo ros2 launch ins401_ros2 imu_publisher.launch.py
```

### Launch with Custom Parameters

```bash
sudo ros2 launch ins401_ros2 imu_publisher.launch.py \
    interface_name:=eth0 \
    target_mac:=00:04:4b:e7:b5:0a \
    publish_rate:=200.0
```

### Launch with Configuration File

```bash
sudo ros2 launch ins401_ros2 imu_publisher_with_config.launch.py
```

### Run Node Directly

```bash
sudo ros2 run ins401_ros2 imu_publisher \
    --ros-args \
    -p interface_name:=eth0 \
    -p target_mac:=00:04:4b:e7:b5:0a
```

## Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `interface_name` | string | "eth0" | Network interface connected to INS401 |
| `target_mac` | string | "00:04:4b:e7:b5:0a" | MAC address of INS401 device |
| `frame_id` | string | "imu_link" | TF frame ID for IMU messages |
| `publish_rate` | double | 200.0 | Publishing rate in Hz (max 200) |
| `save_to_file` | bool | false | Enable saving IMU data to file |
| `output_file` | string | "imu_data.txt" | Output file path for data logging |

## Published Topics

| Topic | Type | Description |
|-------|------|-------------|
| `/ins401/imu/data_raw` | `sensor_msgs/msg/Imu` | Raw IMU data (accel + gyro) |
| `/ins401/imu/accel` | `geometry_msgs/msg/Vector3Stamped` | Accelerometer data only |
| `/ins401/imu/gyro` | `geometry_msgs/msg/Vector3Stamped` | Gyroscope data only |

## Data Format

- **Accelerometer**: Linear acceleration in m/s²
- **Gyroscope**: Angular velocity in rad/s (converted from deg/s)
- **Orientation**: Set to identity quaternion (not available from raw IMU)

## Configuration

Edit the configuration file at `config/imu_params.yaml` to set your default parameters:

```yaml
ins401_imu_publisher:
  ros__parameters:
    interface_name: "eth0"
    target_mac: "00:04:4b:e7:b5:0a"
    frame_id: "imu_link"
    publish_rate: 200.0
    save_to_file: false
    output_file: "imu_data.txt"
```

## Troubleshooting

### Permission Denied Error

The node requires root privileges to create raw sockets:

```bash
sudo ros2 launch ins401_ros2 imu_publisher.launch.py
```

### No Data Received

1. Check network interface name:
   ```bash
   ip addr show
   ```

2. Verify INS401 MAC address:
   ```bash
   sudo tcpdump -i eth0 -e -n
   ```

3. Check if INS401 is sending data:
   ```bash
   sudo tcpdump -i eth0 ether src 00:04:4b:e7:b5:0a
   ```

### Viewing Published Data

```bash
# View IMU messages
ros2 topic echo /ins401/imu/data_raw

# Check publishing rate
ros2 topic hz /ins401/imu/data_raw

# View topic info
ros2 topic info /ins401/imu/data_raw
```

## License

MIT License - See LICENSE file for details