#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <queue>
#include <thread>
#include <unistd.h>

#include "data_type.h"
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

	struct INSSolutionData {
		// TODO
	};

	explicit INSDeviceReceiver(const std::string &iface, const std::string &target_mac, const std::string &local_mac,
							   bool save_to_file);
	~INSDeviceReceiver();

	void Run();
	void Stop();
	bool GetGNSSData(std::vector<GNSSSolutionData> &data, size_t max_count = 10);
	bool GetIMUData(std::vector<RawIMUData> &data, size_t max_count = 500);
	bool isRunning() const { return running_; }

private:
	int sock_fd_;
	std::string interface_name_;
	std::array<uint8_t, 6> target_mac_{};
	std::array<uint8_t, 6> local_mac_{};
	std::atomic<bool> running_{ false };
	const size_t gnss_hz_ = 1;
	const size_t imu_hz_ = 100;
	const size_t buffer_size_ = { 8 * 1024 };
	std::queue<GNSSSolutionData> gnss_queue_;
	const size_t max_gnss_queue_size_ = gnss_hz_ * 5 * 60;	// 5 minutes of GNSS data
	std::queue<RawIMUData> imu_queue_;
	const size_t max_imu_queue_size_ = imu_hz_ * 5 * 60;	// 5 minutes of IMU data
	mutable std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::thread writer_thread_;
	std::ofstream gnss_file_;
	std::ofstream imu_file_;
	bool save_to_file_;

	// Write buffers
	const size_t gnss_batch_size_ = gnss_hz_ * 5;  // 5 seconds of GNSS data at 1Hz
	const size_t imu_batch_size_ = imu_hz_ * 5;	   // 5 seconds of IMU data at 100Hz
	const size_t write_buffer_size_ = 128 * 1024;  // 128 KB buffer
	std::vector<char> gnss_file_buffer_;
	std::vector<char> imu_file_buffer_;
	size_t last_flush_time_ = 0;

	bool Initialize();
	void InitializeFiles();
	void ReceiveLoop();
	void VerifyData(const uint8_t *data, size_t len);
	void ProcessGNSSSolutionData(const uint8_t *packet, uint32_t data_length);
	void ProcessRawIMUData(const uint8_t *packet, uint32_t data_length);
	void WriterThread();
	void WriteIMUBatch(const std::vector<RawIMUData> &batch, std::string &buffer);
	void WriteGNSSBatch(const std::vector<GNSSSolutionData> &batch, std::string &buffer);
	// void WriteIMUBinary(const std::vector<RawIMUData> &batch);
};
