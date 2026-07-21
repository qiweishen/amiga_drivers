#ifndef LMS4XXX_DRIVER_H
#define LMS4XXX_DRIVER_H

#include <atomic>
#include <memory>
#include <system_error>

#include "lms4xxx_callbacks.h"
#include "lms4xxx_config.h"
#include "lms4xxx_scan_data.h"
#include "lms4xxx_statistics.h"


namespace LMS4xxx {

	// Forward declarations (implementation details).
	class FrameReceiver;
	class CoLaBCodec;
	class CommandBuilder;
	class ScanDataParser;


	// Main driver class for the SICK LMS4xxx 2D LiDAR sensor.
	class LMS4xxxDriver {
	public:
		explicit LMS4xxxDriver(const DriverConfig &config);
		~LMS4xxxDriver();

		// Non-copyable, non-movable (owns threads and I/O resources).
		LMS4xxxDriver(const LMS4xxxDriver &) = delete;
		LMS4xxxDriver &operator=(const LMS4xxxDriver &) = delete;
		LMS4xxxDriver(LMS4xxxDriver &&) = delete;
		LMS4xxxDriver &operator=(LMS4xxxDriver &&) = delete;

		// Establish TCP connection to the sensor.
		[[nodiscard]] std::error_code Connect();

		// Send configuration commands to the sensor.
		[[nodiscard]] std::error_code Configure();

		// Enable continuous scan data streaming (sEN LMDscandata 1).
		[[nodiscard]] std::error_code StartScanning();

		// Disable continuous scan data streaming (sEN LMDscandata 0).
		std::error_code StopScanning();

		// Close the TCP connection and release resources.
		void Disconnect();

		// Register callback for scan data. Called from the parse thread.
		void SetScanCallback(ScanDataCallback callback);

		// Register callback for connection state changes.
		void SetConnectionCallback(ConnectionStateCallback callback);

		// Register callback for errors (CRC, protocol, device errors).
		void SetErrorCallback(ErrorCallback callback);

		// Current connection state.
		[[nodiscard]] ConnectionState GetConnectionState() const;

		// True if connected to the sensor.
		[[nodiscard]] bool IsConnected() const;

		// True if actively receiving scan data.
		[[nodiscard]] bool IsScanning() const;

		// Get a snapshot of the runtime statistics.
		[[nodiscard]] DriverStatistics::Snapshot GetStatistics() const;

		// Log current statistics at INFO level.
		void LogStatistics() const;

		// Request a single scan frame (sRN LMDscandata). Blocks until response received or timeout.
		[[nodiscard]] std::error_code PollSingleScan(ScanData &out);

		// Start device measurement (sMN LMCstartmeas). Requires login.
		[[nodiscard]] std::error_code StartMeasurement();

		// Stop device measurement (sMN LMCstopmeas). Requires login.
		[[nodiscard]] std::error_code StopMeasurement();

		// Enter standby mode (sMN LMCstandby). Shuts off laser, motor keeps running.
		[[nodiscard]] std::error_code Standby();

		// Reboot the device (sMN mSCreboot).
		[[nodiscard]] std::error_code RebootDevice();

		// Update the scan configuration. Must call Configure() afterwards.
		void SetScanConfig(const ScanConfig &config);

		// Get the current driver configuration.
		[[nodiscard]] const DriverConfig &GetConfig() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

}  // namespace LMS4xxx

#endif	// LMS4XXX_DRIVER_H
