/**
 * @file ins_receiver.h
 * @brief INS401 device data receiver with multi-threaded processing and file logging.
 *
 * Provides real-time reception and processing of INS401 sensor data streams
 * including GNSS solutions, IMU measurements, diagnostics, and RTCM corrections.
 * Supports both ROS2 integration via data queues and optional file-based logging.
 *
 * @author Qiwei
 * @date 2025
 *
 * @note This class is designed for high-frequency sensor data (up to 100Hz IMU)
 *       and uses lock-free patterns where possible for performance.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "ethernet_socket.h"



/**
 * @class INSDeviceReceiver
 * @brief Receives and processes data streams from an INS401 device.
 *
 * This class manages bidirectional Ethernet communication with an INS401 device,
 * handling multiple concurrent data streams:
 * - GNSS position/velocity solutions (1 Hz)
 * - Raw IMU accelerometer/gyroscope data (100 Hz)
 * - Device diagnostic messages (1 Hz)
 * - RTCM rover correction data (10 Hz)
 * - NMEA ASCII messages (variable rate)
 *
 * @par Architecture
 * The receiver uses a multi-threaded architecture:
 * - Main receive loop: Captures packets from Ethernet socket
 * - Writer thread: Asynchronously flushes data to files (if enabled)
 * - Thread-safe queues: Buffer data for ROS2 consumers
 *
 * @par Thread Safety
 * - GetGNSSData() and GetIMUData() are thread-safe for concurrent access.
 * - Run() should only be called from a single thread.
 * - Destructor safely terminates all threads.
 *
 * @note Requires CAP_NET_RAW capability or root privileges for raw socket access.
 *
 * @par Example Usage
 * @code
 * // Create receiver with file logging enabled
 * INSDeviceReceiver receiver(
 *     "eth0",                          // Network interface
 *     "00:11:22:33:44:55",             // Device MAC
 *     "AA:BB:CC:DD:EE:FF",             // Local MAC
 *     true,                            // Enable file saving
 *     "/data/ins_logs"                 // Output folder
 * );
 *
 * // Start receiving in a separate thread
 * std::thread rx_thread([&receiver]() { receiver.Run(); });
 *
 * // Consume data from another thread
 * while (receiver.isRunning()) {
 *     std::vector<INSDeviceReceiver::GNSSSolutionData> gnss_data;
 *     if (receiver.GetGNSSData(gnss_data, 10)) {
 *         for (const auto& fix : gnss_data) {
 *             // Publish to ROS2 or process
 *         }
 *     }
 *     std::this_thread::sleep_for(std::chrono::milliseconds(100));
 * }
 *
 * rx_thread.join();
 * @endcode
 *
 * @see EthernetSocket for underlying socket implementation
 */
class INSDeviceReceiver {
public:
	/**
	 * @defgroup DataStructures Sensor Data Structures
	 * @brief Parsed sensor data structures for INS401 messages.
	 * @{
	 */

	/**
	 * @struct GNSSSolutionData
	 * @brief Parsed GNSS position and velocity solution.
	 *
	 * Contains the complete GNSS navigation solution including position,
	 * velocity, accuracy estimates, and satellite geometry information.
	 * Corresponds to message ID 0x0A02.
	 */
	struct GNSSSolutionData {
		std::uint16_t gps_week;				  ///< GPS week number since Jan 6, 1980.
		std::uint32_t gps_millisecs;		  ///< Milliseconds from start of GPS week.
		std::uint8_t position_type;			  ///< Solution type (0=None, 1=Single, 4=Fixed RTK, etc.).
		double latitude;					  ///< Geodetic latitude in degrees [-90, 90].
		double longitude;					  ///< Geodetic longitude in degrees [-180, 180].
		double height;						  ///< Ellipsoidal height in meters.
		float latitude_std;					  ///< Latitude standard deviation in meters.
		float longitude_std;				  ///< Longitude standard deviation in meters.
		float height_std;					  ///< Height standard deviation in meters.
		std::uint8_t num_of_SVs;			  ///< Number of satellites tracked.
		std::uint8_t num_of_SVs_in_solution;  ///< Number of satellites used in solution.
		float hdop;							  ///< Horizontal dilution of precision.
		float diffage;						  ///< Differential correction age in seconds.
		float north_vel;					  ///< North velocity in m/s (NED frame).
		float east_vel;						  ///< East velocity in m/s (NED frame).
		float up_vel;						  ///< Up velocity in m/s (NED frame, positive up).
		float north_vel_std;				  ///< North velocity standard deviation in m/s.
		float east_vel_std;					  ///< East velocity standard deviation in m/s.
		float up_vel_std;					  ///< Up velocity standard deviation in m/s.
	};

	/**
	 * @struct INSSolutionData
	 * @brief Parsed INS navigation solution.
	 *
	 * Contains the integrated INS/GNSS navigation solution including
	 * position, velocity, and attitude. Corresponds to message ID 0x0A03.
	 *
	 * @todo Complete structure definition based on INS401 protocol specification.
	 */
	struct INSSolutionData {
		// TODO: Add INS solution fields
	};

	/**
	 * @struct DiagnosticMessage
	 * @brief Device diagnostic and health status.
	 *
	 * Contains device status flags and temperature readings for monitoring
	 * system health. Corresponds to message ID 0x0A05.
	 *
	 * @see INS401 User Manual Table 7 for device_status bit definitions.
	 */
	struct DiagnosticMessage {
		std::uint16_t gps_week;				///< GPS week number.
		std::uint32_t gps_millisecs;		///< Milliseconds from start of GPS week.
		std::array<int, 32> device_status;	///< Device status bit flags (see manual Table 7).
		float imu_temperature;				///< IMU sensor temperature in °C.
		float mcu_temperature;				///< Microcontroller temperature in °C.
		float gnss_chip_temperature;		///< GNSS receiver chip temperature in °C.
	};

	/**
	 * @struct RawIMUData
	 * @brief Raw IMU accelerometer and gyroscope measurements.
	 *
	 * Contains unfiltered inertial measurements from the IMU sensor.
	 * Corresponds to message ID 0x0A01. Output rate is typically 100 Hz.
	 *
	 * @note Measurements are in the IMU body frame, not the vehicle frame.
	 */
	struct RawIMUData {
		std::uint16_t gps_week;		  ///< GPS week number.
		std::uint32_t gps_millisecs;  ///< Milliseconds from start of GPS week.
		float acc_x;				  ///< X-axis acceleration in m/s².
		float acc_y;				  ///< Y-axis acceleration in m/s².
		float acc_z;				  ///< Z-axis acceleration in m/s².
		float gyro_x;				  ///< X-axis angular rate in deg/s.
		float gyro_y;				  ///< Y-axis angular rate in deg/s.
		float gyro_z;				  ///< Z-axis angular rate in deg/s.
	};

	/** @} */						  // end of DataStructures

	/**
	 * @brief Constructs an INS device receiver.
	 *
	 * Initializes the Ethernet socket and optionally sets up file logging.
	 * Does not start receiving until Run() is called.
	 *
	 * @param[in] iface              Network interface name (e.g., "eth0").
	 * @param[in] device_mac         Target INS401 device MAC address string
	 *                               (format: "XX:XX:XX:XX:XX:XX").
	 * @param[in] save_to_file       Enable file-based data logging.
	 * @param[in] output_folder_path Directory path for log files (must exist
	 *                               if @p save_to_file is true).
	 *
	 * @throws std::runtime_error If error occurs during socket creation or configuration.
	 *
	 * @pre Network interface must exist and be UP.
	 * @pre Output folder must exist and be writable if file saving is enabled.
	 */
	explicit INSDeviceReceiver(std::string iface, const std::string &device_mac, bool save_to_file, std::string output_folder_path);

	/**
	 * @brief Destructor. Stops all threads and closes resources.
	 *
	 * Signals the receive loop to stop, waits for threads to complete,
	 * flushes any buffered data to files, and closes all file handles.
	 *
	 * @note This is a blocking call that may take up to 1 second for
	 *       threads to terminate gracefully.
	 */
	~INSDeviceReceiver();

	/// @name Deleted Copy/Move Operations
	/// @brief INSDeviceReceiver is non-copyable and non-movable.
	/// @{
	INSDeviceReceiver(const INSDeviceReceiver &) = delete;
	INSDeviceReceiver &operator=(const INSDeviceReceiver &) = delete;
	INSDeviceReceiver(INSDeviceReceiver &&) = delete;
	INSDeviceReceiver &operator=(INSDeviceReceiver &&) = delete;
	/// @}

	/**
	 * @brief Starts the main receive loop.
	 *
	 * Begins receiving and processing packets from the INS401 device.
	 * This method blocks until the receiver is stopped (via destructor
	 * or external signal setting running_ to false).
	 *
	 * @note Call this method from a dedicated thread to avoid blocking
	 *       the main application.
	 *
	 * @post isRunning() returns true until the loop exits.
	 */
	void Run();

	void Stop();

	/**
	 * @brief Checks if the receiver is currently running.
	 * @return true if the receive loop is active, false otherwise.
	 * @note Thread-safe (uses atomic variable).
	 */
	bool isRunning() const { return running_.load(); }

	/**
	 * @name ROS2 Interface Methods
	 * @brief Thread-safe data retrieval for ROS2 integration.
	 * @{
	 */

	/**
	 * @brief Retrieves buffered GNSS solution data.
	 *
	 * Extracts up to @p max_count GNSS solutions from the internal queue.
	 * Thread-safe for concurrent access from ROS2 publisher nodes.
	 *
	 * @param[out] data      Vector to receive GNSS data (cleared before filling).
	 * @param[in]  max_count Maximum number of solutions to retrieve.
	 *
	 * @return true if at least one solution was retrieved, false if queue was empty.
	 *
	 * @note Data is removed from the queue once retrieved.
	 * @note Queue has a maximum capacity of 1 minute of data at 1 Hz.
	 */
	bool GetGNSSData(std::vector<GNSSSolutionData> &data, std::size_t max_count = 10);

	/**
	 * @brief Retrieves buffered raw IMU data.
	 *
	 * Extracts up to @p max_count IMU measurements from the internal queue.
	 * Thread-safe for concurrent access from ROS2 publisher nodes.
	 *
	 * @param[out] data      Vector to receive IMU data (cleared before filling).
	 * @param[in]  max_count Maximum number of measurements to retrieve.
	 *
	 * @return true if at least one measurement was retrieved, false if queue was empty.
	 *
	 * @note Data is removed from the queue once retrieved.
	 * @note Queue has a maximum capacity of 1 minute of data at 100 Hz.
	 * @note Consider using max_count >= 100 for 1-second batches at 100 Hz.
	 */
	bool GetIMUData(std::vector<RawIMUData> &data, std::size_t max_count = 500);

	/** @} */  // end of ROS2 Interface Methods

private:
	/**
	 * @name Socket and Interface Configuration
	 * @{
	 */
	std::shared_ptr<EthernetSocket> socket_ptr_;  ///< Ethernet socket for device communication.
	std::string interface_name_;				  ///< Network interface name.
	MacAddress device_mac_{};					  ///< Target device MAC address.
	MacAddress local_mac_{};					  ///< Local interface MAC address.
	std::atomic<bool> running_{ false };		  ///< Atomic flag for receive loop control.
	/** @} */

	/**
	 * @name Data Rate Constants
	 * @brief Expected data rates for each message type in Hz.
	 * @{
	 */
	const std::size_t gnss_hz_ = 1;			///< GNSS solution output rate (Hz).
	const std::size_t ins_hz_ = 100;		///< INS solution output rate (Hz).
	const std::size_t diagnostic_hz_ = 1;	///< Diagnostic message rate (Hz).
	const std::size_t imu_hz_ = 100;		///< Raw IMU data rate (Hz).
	const std::size_t rtcm_rover_hz_ = 10;	///< RTCM rover data rate (Hz).
	/** @} */

	/**
	 * @name Buffer Configuration
	 * @{
	 */
	const std::size_t buffer_size_ = 64 * 1024;	 ///< Socket receive buffer size (64 KB).
	/** @} */

	/**
	 * @name Data Queues
	 * @brief Thread-safe queues for buffering received data.
	 * @{
	 */
	std::queue<GNSSSolutionData> gnss_queue_;								 ///< GNSS solution queue.
	const std::size_t max_gnss_queue_size_ = 1 * gnss_hz_ * 60;				 ///< Max queue size (1 min).

	std::queue<DiagnosticMessage> diagnostic_queue_;						 ///< Diagnostic message queue.
	const std::size_t max_diagnostic_queue_size_ = 1 * diagnostic_hz_ * 60;	 ///< Max queue size (1 min).

	std::queue<RawIMUData> imu_queue_;										 ///< Raw IMU data queue.
	const std::size_t max_imu_queue_size_ = 1 * imu_hz_ * 60;				 ///< Max queue size (1 min).

	std::queue<std::vector<std::uint8_t>> rtcm_rover_queue_;				 ///< RTCM rover data queue.
	const std::size_t max_rtcm_rover_queue_size_ = 1 * rtcm_rover_hz_ * 60;	 ///< Max queue size (1 min).

	std::queue<std::string> nmea_queue_;									 ///< NMEA message queue.
	const std::size_t max_nmea_queue_size_ = 128;							 ///< Max NMEA messages buffered.

	mutable std::mutex queue_mutex_;										 ///< Mutex protecting all data queues.
	std::condition_variable cv_;											 ///< Condition variable for queue notifications.
	/** @} */

	/**
	 * @name File Logging Configuration
	 * @{
	 */
	bool save_to_file_;				  ///< Flag indicating if file logging is enabled.
	std::string output_folder_path_;  ///< Output directory for log files.
	std::thread writer_thread_;		  ///< Background thread for file writing.

	std::ofstream gnss_file_;		  ///< GNSS solution log file stream.
	std::ofstream diagnostic_file_;	  ///< Diagnostic message log file stream.
	std::ofstream imu_file_;		  ///< Raw IMU data log file stream.
	std::ofstream rtcm_rover_file_;	  ///< RTCM rover data log file stream.
	std::ofstream nmea_file_;		  ///< NMEA message log file stream.
	/** @} */

	/**
	 * @name Write Buffer Configuration
	 * @brief Batch sizes and buffers for efficient file I/O.
	 * @{
	 */
	const std::size_t gnss_write_batch_size_ = gnss_hz_ * 10;			   ///< GNSS batch size (10 sec).
	const std::size_t diagnostic_write_batch_size_ = diagnostic_hz_ * 10;  ///< Diagnostic batch (10 sec).
	const std::size_t imu_write_batch_size_ = imu_hz_ * 10;				   ///< IMU batch size (10 sec).
	const std::size_t rtcm_rover_write_batch_size_ = rtcm_rover_hz_ * 64;  ///< RTCM batch size.
	const std::size_t nmea_write_batch_size_ = 24;						   ///< NMEA batch size.
	const std::size_t write_buffer_size_ = 256 * 1024;					   ///< File buffer size (256 KB).

	std::vector<char> gnss_file_buffer_;								   ///< GNSS file write buffer.
	std::vector<char> diagnostic_file_buffer_;							   ///< Diagnostic file write buffer.
	std::vector<char> imu_file_buffer_;									   ///< IMU file write buffer.
	std::vector<char> rtcm_rover_file_buffer_;							   ///< RTCM file write buffer.
	std::vector<char> nmea_file_buffer_;								   ///< NMEA file write buffer.
	std::size_t last_flush_time_ = 0;									   ///< Timestamp of last file flush.
	/** @} */

	/**
	 * @name Private Methods
	 * @{
	 */

	/**
	 * @brief Initializes output files for data logging.
	 *
	 * Creates and opens all log files in the output folder. Sets up
	 * file buffers for efficient I/O.
	 *
	 * @return true if all files were opened successfully, false otherwise.
	 */
	bool InitializeWritingFiles();

	/**
	 * @brief Main packet receive loop.
	 *
	 * Continuously receives Ethernet frames and dispatches them for
	 * processing based on message type.
	 */
	void ReceiveLoop();

	/**
	 * @brief Validates and parses an incoming data frame.
	 *
	 * Checks frame structure, verifies CRC, and routes to appropriate
	 * message processor.
	 *
	 * @param[in] data Pointer to raw frame data.
	 * @param[in] len  Length of frame in bytes.
	 */
	void VerifyDataFrame(const std::uint8_t *data, std::size_t len);

	/**
	 * @brief Processes a GNSS solution packet (message ID 0x0A02).
	 * @param[in] packet Pointer to packet payload (after Aceinna header).
	 */
	void ProcessGNSSSolutionData(const std::uint8_t *packet);

	/**
	 * @brief Processes a diagnostic message packet (message ID 0x0A05).
	 * @param[in] packet Pointer to packet payload.
	 */
	void ProcessDiagnosticMessage(const std::uint8_t *packet);

	/**
	 * @brief Processes a raw IMU data packet (message ID 0x0A01).
	 * @param[in] packet Pointer to packet payload.
	 */
	void ProcessRawIMUData(const std::uint8_t *packet);

	/**
	 * @brief Processes an RTCM rover data packet (message ID 0x0A06).
	 * @param[in] packet Pointer to packet payload.
	 * @param[in] len    Payload length in bytes.
	 */
	void ProcessRTCMRoverData(const std::uint8_t *packet, std::size_t len);

	/**
	 * @brief Processes an NMEA ASCII message.
	 * @param[in] packet Pointer to null-terminated NMEA string.
	 */
	void ProcessNMEAMessage(const std::uint8_t *packet);

	/**
	 * @brief Background thread function for file writing.
	 *
	 * Periodically drains data queues and writes to files in batches
	 * for optimal I/O performance.
	 */
	void WriterThread();

	/**
	 * @brief Writes a batch of GNSS solutions to file.
	 * @param[in] batch Vector of GNSS solutions to write.
	 */
	void WriteGNSSBatch(const std::vector<GNSSSolutionData> &batch);

	/**
	 * @brief Writes a batch of diagnostic messages to file.
	 * @param[in] batch Vector of diagnostic messages to write.
	 */
	void WriteDiagnosticBatch(const std::vector<DiagnosticMessage> &batch);

	/**
	 * @brief Writes a batch of IMU measurements to file.
	 * @param[in] batch Vector of IMU data to write.
	 */
	void WriteIMUBatch(const std::vector<RawIMUData> &batch);

	/**
	 * @brief Writes a batch of RTCM rover messages to file.
	 * @param[in] batch Vector of raw RTCM message buffers.
	 */
	void WriteRTCMRoverBatch(const std::vector<std::vector<std::uint8_t>> &batch);

	/**
	 * @brief Writes a batch of NMEA messages to file.
	 * @param[in] batch Vector of NMEA strings.
	 */
	void WriteNMEABatch(const std::vector<std::string> &batch);

	/** @} */  // end of Private Methods
};
