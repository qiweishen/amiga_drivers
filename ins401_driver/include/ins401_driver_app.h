/// @file ins_driver_app.h
/// @brief Application wrapper for the INS401 driver lifecycle.
///
/// Encapsulates discovery, receiver, NTRIP client, and initialization monitor
/// into an init/run/shutdown pattern matching the LMS41xxx DriverApp interface.
/// This enables the unified main to run both drivers concurrently.

#ifndef INS_DRIVER_APP_H
#define INS_DRIVER_APP_H

#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "data_type.h"
#include "driver_app.h"
#include "ins401_data_type.h"


// Forward declarations to avoid pulling heavy headers into every translation unit.
namespace INS401 {
	class INSDeviceReceiver;
	class NTRIPClient;
	class NTRIPCallback;
	class InitializationMonitor;
	struct DeviceInfo;
}  // namespace INS401


class Ins401DriverApp final : public Common::IDriverApp {
public:
	// @param config_path Path to the INS401 YAML configuration file.
	explicit Ins401DriverApp(const Common::Config &config);

	~Ins401DriverApp() override;

	// Discover the device, set up receiver/NTRIP threads, and prepare for run().
	// The external_stop predicate is unused: bring-up is short (bounded discovery window).
	// @return true on success, false on fatal error (no device found, config failure, etc.)
	[[nodiscard]] bool init(const std::function<bool()> &external_stop = {}) override;

	// Main loop: blocks until the terminate flag is set.
	void run() override;

	// Graceful shutdown: stops receiver, disconnects NTRIP, joins threads,
	// and logs statistics.
	void shutdown() override;

private:
	std::string config_path_;
	INS401::INSConfig config_{};

	// Device discovered on the network.
	std::unique_ptr<INS401::DeviceInfo> device_;

	// Core components.
	std::shared_ptr<INS401::InitializationMonitor> init_monitor_;
	std::shared_ptr<INS401::INSDeviceReceiver> receiver_;
	std::unique_ptr<INS401::NTRIPClient> ntrip_client_;
	std::unique_ptr<INS401::NTRIPCallback> ntrip_callback_;

	// Internal threads.
	std::thread receiver_thread_;
	std::thread ntrip_thread_;

	std::atomic<bool> shutdown_called_{ false };
};


#endif	// INS_DRIVER_APP_H
