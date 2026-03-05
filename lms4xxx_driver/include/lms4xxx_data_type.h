#ifndef LIDAR_DATA_TYPE_H
#define LIDAR_DATA_TYPE_H

#include <string>

#include "lms4xxx_config.h"


struct LiDARConfig {
	// Instance identification
	std::string position_name;
	std::string hostname;

	// Driver config (loaded from JSON or constructed from YAML)
	LMS4xxx::DriverConfig driver_config;

	// NTP time synchronization
	bool enable_ntp = false;
	std::string ntp_server_ip;
	double sync_time = 1.0;	 // seconds

	// Recording
	std::size_t recording_queue_capacity = 512; // SPSC queue frames (parse thread → write thread)
	std::size_t recording_write_buffer_size = 256 * 1024; // ofstream pubsetbuf size (256 KB)
	std::size_t recording_max_file_bytes = 1ULL * 1024 * 1024 * 1024;  // 1 GB per file

	// Paths (set by main.cpp)
	std::string data_folder_path;
	std::string timestamp;
	std::string config_path;
};

#endif	// LIDAR_DATA_TYPE_H
