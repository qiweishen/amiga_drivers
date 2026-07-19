/// @file ins_driver_app.h
/// @brief Application wrapper for the INS401 driver lifecycle.
///
/// Encapsulates discovery, receiver, NTRIP client, and initialization monitor
/// into an init/run/shutdown pattern matching the LMS41xxx DriverApp interface.
/// This enables the unified main to run both drivers concurrently.

#ifndef INS_DRIVER_APP_H
#define INS_DRIVER_APP_H

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "data_type.h"
#include "ins401_data_type.h"


// Forward declarations to avoid pulling heavy headers into every translation unit.
class INSDeviceDiscover;
class INSDeviceReceiver;
class NTRIPClient;
class NTRIPCallback;
class InitializationMonitor;
class TerminalSpinner;
struct DeviceInfo;


class Ins401DriverApp {
public:
	// @param config_path Path to the INS401 YAML configuration file.
	explicit Ins401DriverApp(const Common::Config &config);

	~Ins401DriverApp();

	Ins401DriverApp(const Ins401DriverApp &) = delete;
	Ins401DriverApp &operator=(const Ins401DriverApp &) = delete;

	// Discover the device, set up receiver/NTRIP threads, and prepare for run().
	// @return true on success, false on fatal error (no device found, config failure, etc.)
	[[nodiscard]] bool init();

	// Main loop: blocks until the terminate flag is set. Polls initialization and shows spinner.
	void run();

	// Graceful shutdown: stops receiver, disconnects NTRIP, joins threads,
	// post-processes binary files, and logs statistics.
	void shutdown();

	// Shared terminate flag. Can be wired to a signal handler or set externally.
	[[nodiscard]] std::atomic<bool> &TerminateFlag() { return terminate_; }

private:
	std::atomic<bool> terminate_{ false };

	std::string config_path_;
	INSConfig config_{};

	// Device discovered on the network.
	std::unique_ptr<DeviceInfo> device_;

	// Core components.
	std::shared_ptr<InitializationMonitor> init_monitor_;
	std::shared_ptr<INSDeviceReceiver> receiver_;
	std::unique_ptr<NTRIPClient> ntrip_client_;
	std::unique_ptr<NTRIPCallback> ntrip_callback_;

	// Internal threads.
	std::thread receiver_thread_;
	std::thread ntrip_thread_;

	std::atomic<bool> shutdown_called_{ false };
};


#endif	// INS_DRIVER_APP_H
