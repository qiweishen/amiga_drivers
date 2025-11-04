#pragma once

#include <unistd.h>
#include <fstream>
#include <queue>
#include <thread>
#include <condition_variable>

#include "tool.h"
#include "data_type.h"



class INSDeviceReceiver {
public:
	INSDeviceReceiver(const std::string &iface, const std::string &mac_addr, bool save_to_file);
	~INSDeviceReceiver();

	void Stop();
	void Run();
	bool GetGNSSData(std::vector<GNSSSolutionData> &data, size_t max_count = 10);
	bool GetIMUData(std::vector<RawIMUData> &data, size_t max_count = 500);
	bool isRunning() const { return running_; }
	void HandleRTCMMessage(const uint8_t *data, size_t size);

private:
	int sock_fd_{};
	uint8_t *COMMAND_START_;
	std::string interface_name_;
	uint8_t target_mac_[6]{};
	std::atomic<bool> running_;
	size_t buffer_size_{64 * 1024};
	std::queue<GNSSSolutionData> gnss_queue_;
	std::queue<RawIMUData> imu_queue_;
	mutable std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::thread writer_thread_;
	std::ofstream gnss_file_;
	std::ofstream imu_file_;
	bool save_to_file_;

	bool Initialize();
	void ReceiveLoop();
	void VerifyData(const uint8_t *data, size_t len);
	void ProcessGNSSSolutionData(const uint8_t *packet);
	void ProcessRawIMUData(const uint8_t *packet);
	void WriterThread();
};
