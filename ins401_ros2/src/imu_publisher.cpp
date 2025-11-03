#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <geometry_msgs/msg/vector3_stamped.hpp>
#include <std_msgs/msg/header.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <thread>
#include <memory>

#include "ins_driver.h"
#include "data_type.h"

class ImuPublisherNode : public rclcpp::Node {
public:
    ImuPublisherNode() : Node("ins401_imu_publisher") {
        // 声明参数
        this->declare_parameter("interface_name", "enp49s0");
        this->declare_parameter("target_mac", "a1:52:8c:95:00:28");
        this->declare_parameter("frame_id", "imu_link");
        this->declare_parameter("publish_rate", 200.0);  // Hz
        this->declare_parameter("save_to_file", false);

        // 获取参数
        std::string interface_name = this->get_parameter("interface_name").as_string();
        std::string target_mac = this->get_parameter("target_mac").as_string();
        frame_id_ = this->get_parameter("frame_id").as_string();
        double publish_rate = this->get_parameter("publish_rate").as_double();
        bool save_to_file = this->get_parameter("save_to_file").as_bool();

        // 创建IMU发布器
        imu_pub_ = this->create_publisher<sensor_msgs::msg::Imu>("imu/data_raw", 10);

        // 创建加速度和角速度单独的发布器（可选）
        accel_pub_ = this->create_publisher<geometry_msgs::msg::Vector3Stamped>("imu/accel", 10);
        gyro_pub_ = this->create_publisher<geometry_msgs::msg::Vector3Stamped>("imu/gyro", 10);

        // 创建INS接收器
        ins_receiver_ = std::make_unique<EthernetINSReceiver>(interface_name, target_mac, save_to_file);

        // 启动接收线程
        receiver_thread_ = std::thread([this]() {
            ins_receiver_->Run();
        });

        // 创建定时器用于发布
        int period_ms = static_cast<int>(1000.0 / publish_rate);
        timer_ = this->create_wall_timer(
            std::chrono::milliseconds(period_ms),
            std::bind(&ImuPublisherNode::publishTimerCallback, this));

        RCLCPP_INFO(this->get_logger(),
            "INS401 IMU Publisher started\n"
            "  Interface: %s\n"
            "  Target MAC: %s\n"
            "  Frame ID: %s\n"
            "  Publish Rate: %.1f Hz\n"
            "  Save to file: %s",
            interface_name.c_str(), target_mac.c_str(), frame_id_.c_str(),
            publish_rate, save_to_file ? "true" : "false");
    }

    ~ImuPublisherNode() {
        if (ins_receiver_) {
            ins_receiver_->Stop();
        }
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

private:
    void publishTimerCallback() {
        // 从INS接收器获取IMU数据
        std::vector<RawIMUData> imu_batch;
        if (!ins_receiver_->GetIMUData(imu_batch, 500)) {
            return;
        }

        // 发布所有获取到的IMU数据
        for (const auto& imu : imu_batch) {
            publishIMU(imu);
        }
    }

    void publishIMU(const RawIMUData& imu) {
        auto imu_msg = sensor_msgs::msg::Imu();

        // 设置时间戳
        // TODO: 将GPS时间转换为ROS时间
        // GPS周从1980年1月6日开始
        // 这里暂时使用当前系统时间
        imu_msg.header.stamp = this->now();
        imu_msg.header.frame_id = frame_id_;

        // 填充加速度数据 (m/s²)
        imu_msg.linear_acceleration.x = imu.acc_x;
        imu_msg.linear_acceleration.y = imu.acc_y;
        imu_msg.linear_acceleration.z = imu.acc_z;

        // 填充角速度数据 (转换 deg/s 到 rad/s)
        constexpr double DEG_TO_RAD = M_PI / 180.0;
        imu_msg.angular_velocity.x = imu.gyro_x * DEG_TO_RAD;
        imu_msg.angular_velocity.y = imu.gyro_y * DEG_TO_RAD;
        imu_msg.angular_velocity.z = imu.gyro_z * DEG_TO_RAD;

        // 方向四元数设为单位四元数（表示未知）
        imu_msg.orientation.w = 1.0;
        imu_msg.orientation.x = 0.0;
        imu_msg.orientation.y = 0.0;
        imu_msg.orientation.z = 0.0;

        // 设置协方差
        // -1表示方向未知，0表示加速度和角速度已知但协方差未知
        imu_msg.orientation_covariance[0] = -1;

        // 加速度和角速度的协方差设为0（表示已知但精度未知）
        for (int i = 0; i < 9; ++i) {
            if (i == 0 || i == 4 || i == 8) {  // 对角线元素
                imu_msg.angular_velocity_covariance[i] = 0.0;
                imu_msg.linear_acceleration_covariance[i] = 0.0;
            } else {
                imu_msg.angular_velocity_covariance[i] = 0.0;
                imu_msg.linear_acceleration_covariance[i] = 0.0;
            }
        }

        // 发布IMU消息
        imu_pub_->publish(imu_msg);

        // 发布单独的加速度和角速度消息（可选）
        auto accel_msg = geometry_msgs::msg::Vector3Stamped();
        accel_msg.header = imu_msg.header;
        accel_msg.vector.x = imu.acc_x;
        accel_msg.vector.y = imu.acc_y;
        accel_msg.vector.z = imu.acc_z;
        accel_pub_->publish(accel_msg);

        auto gyro_msg = geometry_msgs::msg::Vector3Stamped();
        gyro_msg.header = imu_msg.header;
        gyro_msg.vector.x = imu.gyro_x * DEG_TO_RAD;
        gyro_msg.vector.y = imu.gyro_y * DEG_TO_RAD;
        gyro_msg.vector.z = imu.gyro_z * DEG_TO_RAD;
        gyro_pub_->publish(gyro_msg);
    }

    // GPS时间转换函数（可选实现）
    rclcpp::Time gpsTimeToRosTime(uint16_t gps_week, uint32_t gps_millisecs) {
        // GPS epoch: 1980-01-06 00:00:00 UTC
        // Unix epoch: 1970-01-01 00:00:00 UTC
        // Difference: 315964800 seconds

        constexpr int64_t GPS_TO_UNIX_OFFSET = 315964800;
        constexpr int64_t SECONDS_PER_WEEK = 604800;

        // 计算GPS秒数
        int64_t gps_seconds = gps_week * SECONDS_PER_WEEK + gps_millisecs / 1000;

        // 转换为Unix时间戳
        int64_t unix_seconds = gps_seconds + GPS_TO_UNIX_OFFSET;

        // 转换为纳秒
        int64_t nanoseconds = unix_seconds * 1000000000LL + (gps_millisecs % 1000) * 1000000LL;

        return rclcpp::Time(nanoseconds);
    }


    // ROS2发布器
    rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr imu_pub_;
    rclcpp::Publisher<geometry_msgs::msg::Vector3Stamped>::SharedPtr accel_pub_;
    rclcpp::Publisher<geometry_msgs::msg::Vector3Stamped>::SharedPtr gyro_pub_;
    rclcpp::TimerBase::SharedPtr timer_;


    // INS接收器
    std::unique_ptr<EthernetINSReceiver> ins_receiver_;
    std::thread receiver_thread_;


    // 配置参数
    std::string frame_id_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);

    try {
        auto node = std::make_shared<ImuPublisherNode>();
        rclcpp::spin(node);
    } catch (const std::exception& e) {
        RCLCPP_ERROR(rclcpp::get_logger("main"), "Exception: %s", e.what());
    }

    rclcpp::shutdown();
    return 0;
}