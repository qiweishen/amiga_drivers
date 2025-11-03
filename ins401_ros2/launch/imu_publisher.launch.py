from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        # 你的IMU发布节点
        Node(
            package='ins401_ros2',
            executable='imu_publisher',
            name='ins401_imu_publisher',
            parameters=[{
                'interface_name': 'enp49s0',
                'target_mac': 'a1:52:8c:95:00:28',
                'frame_id': 'imu_link',
                'publish_rate': 200.0,
                'save_to_file': False
            }]
        ),

        # IMU Filter (Complementary filter)
        Node(
            package='imu_complementary_filter',
            executable='complementary_filter_node',
            name='imu_filter',
            parameters=[{
                'fixed_frame': 'world',
                'use_mag': False,
                'publish_tf': True,
                'reverse_tf': False,
                'constant_dt': 0.0,
                'publish_debug_topics': False,
                'gain_acc': 0.01,
                'do_bias_estimation': True,
                'bias_alpha': 0.01,
                'do_adaptive_gain': True,
                'orientation_stddev': 0.0
            }],
            remappings=[
                ('imu/data_raw', '/imu/data_raw'),
                ('imu/data', '/imu/data')
            ]
        ),

        # 静态TF：world到map
        Node(
            package='tf2_ros',
            executable='static_transform_publisher',
            arguments=['0', '0', '0', '0', '0', '0', 'map', 'world']
        ),

        # RViz2
        Node(
            package='rviz2',
            executable='rviz2',
            arguments=['-d', 'path/to/your/config.rviz']
        )
    ])
