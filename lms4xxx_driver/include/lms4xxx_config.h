#ifndef LMS4XXX_CONFIG_H
#define LMS4XXX_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>


namespace LMS4xxx {

	// Device connection parameters. Protocol is hardcoded to CoLa B (TCP port 2111).
	struct DeviceConfig {
		std::string ip = "192.168.0.1";
		std::uint16_t port = 2111;	///< CoLa B binary protocol port
	};


	// Scan data content and range configuration.
	struct ScanConfig {
		// Angular range (degrees)
		double start_angle_deg = 55.0;			 ///< Start angle in degrees
		double stop_angle_deg = 125.0;			 ///< Stop angle in degrees
		double angular_resolution_deg = 0.0833;	 ///< 1/12 degree

		// Channel enable flags
		bool enable_distance = true;		  ///< DIST1 channel
		bool enable_rssi = true;			  ///< RSSI1 channel (signal strength)
		bool enable_reflectance = false;	  ///< REFL1 channel (calibrated reflectance %)
		bool enable_angle_correction = true;  ///< ANGL1 channel (per-point angle correction)
		bool enable_quality = true;			  ///< QLTY1 channel (8-bit quality bitfield)

		std::uint16_t output_rate = 1;		  ///< Output every Nth scan (1 = every scan)

		// Convert start angle to device units (1/10000 degrees).
		[[nodiscard]] std::int32_t StartAngleDevice() const { return static_cast<std::int32_t>(start_angle_deg * 10000.0); }

		// Convert stop angle to device units (1/10000 degrees).
		[[nodiscard]] std::int32_t StopAngleDevice() const { return static_cast<std::int32_t>(stop_angle_deg * 10000.0); }

		// Convert angular resolution to device units (1/10000 degrees).
		[[nodiscard]] std::uint16_t AngularResolutionDevice() const {
			return static_cast<std::uint16_t>(angular_resolution_deg * 10000.0);
		}
	};


	// Time synchronization role for TSCRole command.
	enum class TscRole : std::uint8_t {
		kOff = 0,	  ///< Time sync disabled
		kClient = 1,  ///< NTP client mode
		kServer = 2,  ///< NTP server mode
	};


	// NTP time synchronization configuration.
	struct NTPConfig {
		bool enable = false;
		TscRole role = TscRole::kClient;
		std::string server_ip = "192.168.0.100";
		std::uint32_t update_interval_s = 1;
	};


	// Network and threading parameters for zero-loss operation.
	struct NetworkConfig {
		// Socket
		std::size_t recv_buffer_bytes = 4 * 1024 * 1024;  ///< SO_RCVBUF size (4 MB)
		bool tcp_keepalive = true;
		int keepalive_idle_s = 10;
		int keepalive_interval_s = 5;
		int keepalive_count = 3;

		// Ring buffer
		std::size_t ring_buffer_frames = 1024;	///< SPSC ring buffer capacity (frames)

		// Receive thread
		int receive_thread_priority = 99;  ///< SCHED_FIFO priority (1-99)
		int receive_thread_cpu = -1;	   ///< CPU affinity (-1 = no pinning)

		// Timeouts
		int connect_timeout_ms = 5000;
		int response_timeout_ms = 2000;
	};


	// Aggregate driver configuration.
	struct DriverConfig {
		DeviceConfig device;
		ScanConfig scan;
		NTPConfig ntp;
		NetworkConfig network;

		// Validate configuration parameters.
		[[nodiscard]] std::error_code Validate() const;
	};

}  // namespace LMS4xxx

#endif	// LMS4XXX_CONFIG_H
