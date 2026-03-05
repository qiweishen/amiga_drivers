#ifndef INS_RECEIVER_H
#define INS_RECEIVER_H

#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "ins401_data_type.h"
#include "ins401_ethernet_socket.h"


class InitializationMonitor;
class NTRIPClient;


// INS401 receiver: writes raw binary during collection.
class INSDeviceReceiver {
public:
	explicit INSDeviceReceiver(std::string iface, std::string device_mac, const INSConfig &config);

	~INSDeviceReceiver();

	INSDeviceReceiver(const INSDeviceReceiver &) = delete;

	INSDeviceReceiver &operator=(const INSDeviceReceiver &) = delete;

	INSDeviceReceiver(INSDeviceReceiver &&) = delete;

	INSDeviceReceiver &operator=(INSDeviceReceiver &&) = delete;

	void Run();

	void Stop();

	[[nodiscard]] bool IsRunning() const { return running_.load(); }

	using ImuCallback = std::function<void(const RawIMUData &)>;
	using GnssCallback = std::function<void(const GNSSSolutionData &)>;

	// For static initialization use case
	void SetImuCallback(ImuCallback callback) {
		std::scoped_lock lock(callback_mutex_);
		imu_callback_ = std::move(callback);
	}

	// For real time monitoring RTK status use case
	void SetGnssCallback(GnssCallback callback) {
		std::scoped_lock lock(callback_mutex_);
		gnss_callback_ = std::move(callback);
	}

	void SetInitializationMonitor(InitializationMonitor *initializer);

	void SetNtripClient(NTRIPClient *client);

	// Parse a GGA NMEA sentence into BLH coordinates (lat_rad, lon_rad, ellipsoidal_height_m)
	// suitable for Tool::Earth::ComputeGravity. Returns std::nullopt on parse failure or no fix.
	[[nodiscard]] static std::optional<Eigen::Vector3d> ParseGgaCoordinates(const std::string &gga);

	// Statistics
	struct GpsTimeRange {
		std::uint16_t first_week = 0;
		std::uint32_t first_ms = 0;
		std::uint16_t last_week = 0;
		std::uint32_t last_ms = 0;
	};

	struct Statistics {
		size_t gnss_packets = 0;
		size_t ins_packets = 0;
		size_t imu_packets = 0;
		size_t diagnostic_packets = 0;
		size_t rtcm_rover_packets = 0;
		size_t nmea_messages = 0;

		size_t gnss_crc_errors = 0;
		size_t ins_crc_errors = 0;
		size_t imu_crc_errors = 0;
		size_t diagnostic_crc_errors = 0;
		size_t rtcm_rover_crc_errors = 0;
		size_t nmea_checksum_errors = 0;

		size_t total_bytes_received = 0;
	};

	[[nodiscard]] Statistics GetStatistics() const;

	void LogStatistics() const;

private:
	std::shared_ptr<EthernetSocket> socket_ptr_;
	std::string interface_name_;
	MacAddress device_mac_{};
	MacAddress local_mac_{};
	std::atomic<bool> running_{ false };

	const std::size_t buffer_size_ = 64 * 1024;

	mutable std::mutex callback_mutex_;
	ImuCallback imu_callback_;
	GnssCallback gnss_callback_;

	std::string output_folder_path_;
	std::string timestamp_;

	// Binary files written during collection (raw payloads)
	std::ofstream gnss_bin_file_;
	std::ofstream ins_bin_file_;
	std::ofstream imu_bin_file_;
	std::ofstream diagnostic_bin_file_;

	// Direct-write files (no post-processing needed)
	std::ofstream rtcm_rover_file_;
	std::ofstream nmea_file_;

	const std::size_t write_buffer_size_ = 256 * 1024;

	std::vector<char> gnss_bin_buffer_;
	std::vector<char> ins_bin_buffer_;
	std::vector<char> imu_bin_buffer_;
	std::vector<char> diagnostic_bin_buffer_;
	std::vector<char> rtcm_rover_file_buffer_;
	std::vector<char> nmea_file_buffer_;

	// Binary file paths
	std::string gnss_bin_path_;
	std::string ins_bin_path_;
	std::string imu_bin_path_;
	std::string diagnostic_bin_path_;

	bool check_gnss_;
	double rtk_horizontal_std_;
	// Hysteresis state machine for GNSS status monitoring:
	// A state transition (e.g. RTK_FIXED gained/lost) must persist for N consecutive
	// frames before it is accepted, preventing log spam from transient fluctuations.
	static constexpr std::uint8_t GnssTransitionConfirmFrames_ = 3;
	bool gnss_state_initialized_ = false;
	bool stable_rtk_fixed_ = false;
	bool stable_std_converged_ = false;
	bool pending_rtk_fixed_ = false;
	bool pending_std_converged_ = false;
	std::uint8_t pending_rtk_count_ = 0;
	std::uint8_t pending_std_count_ = 0;

	// Statistics tracking (lock-free for high-frequency packet handlers)
	std::atomic<size_t> stat_gnss_packets_{ 0 };
	std::atomic<size_t> stat_ins_packets_{ 0 };
	std::atomic<size_t> stat_imu_packets_{ 0 };
	std::atomic<size_t> stat_diagnostic_packets_{ 0 };
	std::atomic<size_t> stat_rtcm_rover_packets_{ 0 };
	std::atomic<size_t> stat_nmea_messages_{ 0 };
	std::atomic<size_t> stat_gnss_crc_errors_{ 0 };
	std::atomic<size_t> stat_ins_crc_errors_{ 0 };
	std::atomic<size_t> stat_imu_crc_errors_{ 0 };
	std::atomic<size_t> stat_diagnostic_crc_errors_{ 0 };
	std::atomic<size_t> stat_rtcm_rover_crc_errors_{ 0 };
	std::atomic<size_t> stat_nmea_checksum_errors_{ 0 };
	std::atomic<size_t> stat_total_bytes_received_{ 0 };

	Eigen::Vector3d first_gga_blh_;
	std::atomic<bool> first_gga_blh_ready_{ false };
	std::atomic<InitializationMonitor *> initialization_monitor_{ nullptr };

	bool use_vrs_;
	std::atomic<NTRIPClient *> ntrip_client_{ nullptr };

	bool InitializeWritingFiles();

	void CloseAllFiles();

	void ReceiveLoop();

	void VerifyDataFrame(const std::uint8_t *data, std::size_t len);

	// Real-time handlers: CRC check + binary write (+ optional callback/queue)
	void HandleGNSSSolutionPacket(const std::uint8_t *packet);

	void HandleINSSolutionPacket(const std::uint8_t *packet);

	void HandleDiagnosticPacket(const std::uint8_t *packet);

	void HandleRawIMUPacket(const std::uint8_t *packet);

	void HandleRTCMRoverPacket(const std::uint8_t *packet, std::size_t len);

	void HandleNMEAMessage(const std::uint8_t *packet, std::size_t max_len);

	// Binary-to-struct parsing (shared between real-time callbacks and post-processing)
	static GNSSSolutionData ParseGNSSSolutionData(const std::uint8_t *payload);

	static INSSolutionData ParseINSSolutionData(const std::uint8_t *payload);

	static DiagnosticMessage ParseDiagnosticMessage(const std::uint8_t *payload);

	static RawIMUData ParseRawIMUData(const std::uint8_t *payload);

	// static

	static bool IsGgaSentence(const std::string &nmea);

	void HandleGgaMessage(const std::string &nmea);

	void MonitorGNSSStatus(GNSSSolutionData &gnss);
};


#endif
