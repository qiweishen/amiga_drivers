#ifndef LIDAR_DRIVER_APP_H
#define LIDAR_DRIVER_APP_H

#include <atomic>
#include <memory>
#include <string>

#include "lms4xxx_data_type.h"
#include "lms4xxx_driver.h"
#include "lms4xxx_scan_record_writer.h"


class LidarDriverApp {
public:
	explicit LidarDriverApp(LiDARConfig config);
	~LidarDriverApp();

	// Non-copyable, non-movable.
	LidarDriverApp(const LidarDriverApp &) = delete;
	LidarDriverApp &operator=(const LidarDriverApp &) = delete;
	LidarDriverApp(LidarDriverApp &&) = delete;
	LidarDriverApp &operator=(LidarDriverApp &&) = delete;

	// Initialize the driver: load config, create LMS4xxxDriver, connect.
	[[nodiscard]] bool init();

	// Run the driver (blocks until terminate_flag is set).
	void run();

	// Graceful shutdown: stop scanning, disconnect.
	void shutdown();

	// Shared termination flag (set by main.cpp signal handler).
	[[nodiscard]] std::atomic<bool> &TerminateFlag() { return terminate_; }

private:
	struct Impl {
		std::unique_ptr<LMS4xxx::LMS4xxxDriver> driver;
		std::unique_ptr<LMS4xxx::ScanRecordWriter> writer;
		std::string instance_name;	// e.g., "front_left"
	};

	std::unique_ptr<Impl> impl_;
	std::atomic<bool> terminate_{ false };
	LiDARConfig config_;
};

#endif	// LIDAR_DRIVER_APP_H
