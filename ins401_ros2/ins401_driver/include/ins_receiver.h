#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <queue>
#include <thread>

#include "tool.h"



class INSDeviceReceiver {
public:
	struct GNSSSolutionData {
		uint16_t gps_week;
		uint32_t gps_millisecs;	 // ms
		uint8_t position_type;
		double latitude;		 // deg
		double longitude;
		double height;			 // m
		float latitude_std;		 // m
		float longitude_std;
		float height_std;
		uint8_t num_of_SVs;
		uint8_t num_of_SVs_in_solution;
		float hdop;
		float diffage;	  // s
		float north_vel;  // m/s
		float east_vel;
		float up_vel;
		float north_vel_std;
		float east_vel_std;
		float up_vel_std;
	};

	struct INSSolutionData {
		// TODO
	};

	struct DiagnosticMessage {
		uint16_t gps_week;
		uint32_t gps_millisecs;				// ms
		std::array<int, 32> device_status;	// Refer to INS401 user manual Table 7
		float imu_temperature;				// °C
		float mcu_temperature;				// °C
		float gnss_chip_temperature;		// °C
	};

	struct RawIMUData {
		uint16_t gps_week;
		uint32_t gps_millisecs;	 // ms
		float acc_x;			 // m/s²
		float acc_y;
		float acc_z;
		float gyro_x;			 // deg/s
		float gyro_y;
		float gyro_z;
	};



	explicit INSDeviceReceiver(const std::string &iface, const std::string &target_mac, const std::string &local_mac,
							   bool save_to_file);
	~INSDeviceReceiver();

	void Run();
	void Stop();
	bool isRunning() const { return running_; }

	// ROS2 interface
	bool GetGNSSData(std::vector<GNSSSolutionData> &data, size_t max_count = 10);
	bool GetIMUData(std::vector<RawIMUData> &data, size_t max_count = 500);

private:
	int sock_fd_;
	std::string interface_name_;
	std::array<uint8_t, 6> target_mac_{};
	std::array<uint8_t, 6> local_mac_{};
	std::atomic<bool> running_{ false };
	const size_t gnss_hz_ = 1;
	const size_t ins_hz_ = 100;
	const size_t diagnostic_hz_ = 1;
	const size_t imu_hz_ = 100;
	const size_t rtcm_rover_hz_ = 10;
	const size_t buffer_size_ = { 64 * 1024 };							// 64 KB buffer
	std::queue<GNSSSolutionData> gnss_queue_;
	const size_t max_gnss_queue_size_ = 1 * gnss_hz_ * 60;				// 1 minutes of GNSS data
	std::queue<DiagnosticMessage> diagnostic_queue_;
	const size_t max_diagnostic_queue_size_ = 1 * diagnostic_hz_ * 60;	// 1 minutes of diagnostic data
	std::queue<RawIMUData> imu_queue_;
	const size_t max_imu_queue_size_ = 1 * imu_hz_ * 60;				// 1 minutes of IMU data
	std::queue<std::vector<uint8_t>> rtcm_rover_queue_;
	const size_t max_rtcm_rover_queue_size_ = 1 * rtcm_rover_hz_ * 60;	// 1 minutes of RTCM rover messages
	std::queue<std::string> nmea_queue_;
	const size_t max_nmea_queue_size_ = 128;							// 128 NMEA messages
	mutable std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::thread writer_thread_;
	std::ofstream gnss_file_;
	std::ofstream diagnostic_file_;
	std::ofstream imu_file_;
	std::ofstream rtcm_rover_file_;
	std::ofstream nmea_file_;
	bool save_to_file_;

	// Write buffers
	const size_t gnss_write_batch_size_ = gnss_hz_ * 10;			  // 10 seconds of GNSS data at 1Hz
	const size_t diagnostic_write_batch_size_ = diagnostic_hz_ * 10;  // 10 seconds of diagnostic data at 1Hz
	const size_t imu_write_batch_size_ = imu_hz_ * 10;				  // 10 seconds of IMU data at 100Hz
	const size_t rtcm_rover_write_batch_size_ = rtcm_rover_hz_ * 64;  // 10 seconds of RTCM rover messages
	const size_t nmea_write_batch_size_ = 24;						  // 24 NMEA messages
	const size_t write_buffer_size_ = 256 * 1024;					  // 256 KB buffer
	std::vector<char> gnss_file_buffer_;
	std::vector<char> diagnostic_file_buffer_;
	std::vector<char> imu_file_buffer_;
	std::vector<char> rtcm_rover_file_buffer_;
	std::vector<char> nmea_file_buffer_;
	size_t last_flush_time_ = 0;

	bool Initialize();
	void InitializeWritingFiles();
	void ReceiveLoop();
	void VerifyDataFrame(const uint8_t *data, size_t len);
	void ProcessGNSSSolutionData(const uint8_t *packet);
	void ProcessDiagnosticMessage(const uint8_t *packet);
	void ProcessRawIMUData(const uint8_t *packet);
	void ProcessRTCMRoverData(const uint8_t *packet, size_t len);
	void ProcessNMEAMessage(const uint8_t *packet);
	void WriterThread();
	void WriteGNSSBatch(const std::vector<GNSSSolutionData> &batch);
	void WriteDiagnosticBatch(const std::vector<DiagnosticMessage> &batch);
	void WriteIMUBatch(const std::vector<RawIMUData> &batch);
	void WriteRTCMRoverBatch(const std::vector<std::vector<uint8_t>> &batch);
	void WriteNMEABatch(const std::vector<std::string> &batch);
	// void WriteIMUBinary(const std::vector<RawIMUData> &batch);
};
