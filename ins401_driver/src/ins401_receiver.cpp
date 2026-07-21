#include "ins401_receiver.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <net/if.h>
#include <spdlog/fmt/std.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <utility>
#include <vector>

#include "initialization_monitor.h"
#include "ins401_ntrip_client.h"
#include "ins401_protocol.h"
#include "ins401_tool.h"
#include "logger.h"
#include "utility.h"


namespace INS401 {
namespace {
	constexpr std::string_view kModule = "INS401Receiver";
	Common::DriverLog g_log{ std::string(kModule) };
}  // namespace


INSDeviceReceiver::INSDeviceReceiver(std::string iface, std::string device_mac, const INSConfig &config) :
	interface_name_(std::move(iface)) {
	device_mac_ = Ethernet::FormatMACAddress(std::move(device_mac));
	socket_ptr_ = std::make_shared<EthernetSocket>(interface_name_, device_mac_, buffer_size_, true);

	use_vrs_ = config.use_vrs;
	check_gnss_ = config.enable_gnss_checking;
	rtk_horizontal_std_ = config.gnss_horizontal_std_threshold;

	output_folder_path_ = config.data_folder_path;
	timestamp_ = config.timestamp;
}


INSDeviceReceiver::~INSDeviceReceiver() {
	Stop();
	CloseAllFiles();
}


void INSDeviceReceiver::Run() {
	if (!InitializeWritingFiles()) {
		// Runs inside receiver_thread_ whose catch in the driver app logs at
		// App level and sets the terminate flag: recording without open files
		// would silently lose the whole session. The open failure itself is
		// already logged; throw plainly so the app logs exactly once
		throw std::runtime_error("Failed to open recording files");
	}
	running_.store(true);
	ReceiveLoop();
}


void INSDeviceReceiver::Stop() {
	running_.store(false);
}


void INSDeviceReceiver::SetInitializationMonitor(InitializationMonitor *initializer) {
	initialization_monitor_.store(initializer, std::memory_order_release);
}


void INSDeviceReceiver::SetNtripClient(NTRIPClient *client) {
	ntrip_client_.store(client, std::memory_order_release);
}


void INSDeviceReceiver::ReceiveLoop() {
	try {
		while (running_.load()) {
			auto frames = socket_ptr_->ReceiveBatch(64);
			if (frames.empty()) {
				auto response = socket_ptr_->Receive(100);
				if (response && !response->empty()) {
					VerifyDataFrame(response->data(), response->size());
				}
			} else {
				for (const auto &frame: frames) {
					VerifyDataFrame(frame.data(), frame.size());
				}
			}
		}
	} catch (const std::exception &e) {
		g_log.error("ReceiveLoop exception: {}", e.what());
		running_.store(false);
	}
}


// Parse an Ethernet frame containing an Aceinna binary packet:
// [0x5555(2) | MsgID(2) | PayloadLen(4) | Payload(N) | CRC16(2)]
// or an NMEA ASCII sentence starting with '$'.
void INSDeviceReceiver::VerifyDataFrame(const uint8_t *data, const size_t len) {
	if (len < 60) {
		return;
	}

	stat_total_bytes_received_.fetch_add(len, std::memory_order_relaxed);

	const uint8_t *packet = data + kEthernetHeaderSize;
	if (packet[0] == COMMAND_START_BYTES[0] && packet[1] == COMMAND_START_BYTES[1]) {
		const uint16_t recv_msg_id = packet[2] | (packet[3] << 8);
		const uint32_t data_length = packet[4] | (packet[5] << 8) | (packet[6] << 16) | (packet[7] << 24);
		switch (recv_msg_id) {
			case GNSS_SOLUTION_PACKET_MESSAGE_ID:
				if (data_length == GNSS_SOLUTION_PACKET_LENGTH) {
					HandleGNSSSolutionPacket(packet);
				} else {
					g_log.warn("Invalid GNSS solution data length: {}, expected: {}", data_length, GNSS_SOLUTION_PACKET_LENGTH);
				}
				break;
			case INS_SOLUTION_PACKET_MESSAGE_ID:
				if (data_length == INS_SOLUTION_PACKET_LENGTH) {
					HandleINSSolutionPacket(packet);
				} else {
					g_log.warn("Invalid INS solution data length: {}, expected: {}", data_length, INS_SOLUTION_PACKET_LENGTH);
				}
				break;
			case DIAGNOSTIC_MESSAGE_ID:
				if (data_length == DIAGNOSTIC_MESSAGE_LENGTH) {
					HandleDiagnosticPacket(packet);
				} else {
					g_log.warn("Invalid diagnostic message length: {}, expected: {}", data_length, DIAGNOSTIC_MESSAGE_LENGTH);
				}
				break;
			case RAW_IMU_DATA_MESSAGE_ID:
				if (data_length == RAW_IMU_DATA_LENGTH) {
					HandleRawIMUPacket(packet);
				} else {
					g_log.warn("Invalid raw IMU data length: {}, expected: {}", data_length, RAW_IMU_DATA_LENGTH);
				}
				break;
			case RTCM_ROVER_DATA_MESSAGE_ID:
				if (data_length >= 1 && data_length <= RTCM_ROVER_DATA_LENGTH_MAX) {
					HandleRTCMRoverPacket(packet, data_length);
				} else {
					g_log.warn("Invalid RTCM rover data length: {}, expected: 1-{}", data_length, RTCM_ROVER_DATA_LENGTH_MAX);
				}
				break;
			default:
				break;
		}
	} else if (packet[0] == NMEA_ASCII_START) {
		const std::size_t payload_len = len - kEthernetHeaderSize;
		HandleNMEAMessage(packet, payload_len);
	} else {
		return;
	}
}


void INSDeviceReceiver::HandleGNSSSolutionPacket(const uint8_t *packet) {
	constexpr size_t crc_offset = ACEINNA_HEADER_LEN + GNSS_SOLUTION_PACKET_LENGTH;
	const uint16_t recv_crc = packet[crc_offset] | (packet[crc_offset + 1] << 8);  // LSB-first
	const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + GNSS_SOLUTION_PACKET_LENGTH);
	if (recv_crc != calc_crc) {
		stat_gnss_crc_errors_.fetch_add(1, std::memory_order_relaxed);
		g_log.warn("GNSS solution data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}", recv_crc, calc_crc);
		return;
	}

	stat_gnss_packets_.fetch_add(1, std::memory_order_relaxed);

	const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
	if (gnss_bin_file_.IsOpen()) {
		gnss_bin_file_.Write(reinterpret_cast<const char *>(payload), static_cast<std::streamsize>(GNSS_SOLUTION_PACKET_LENGTH));
	}

	// Only parse GNSS packet in real-time under the need of GNSS Checking.
	if (check_gnss_) {
		GNSSSolutionData gnss = ParseGNSSSolutionPayload(payload);
		GnssCallback gnss_cb;
		{
			std::scoped_lock lock(callback_mutex_);
			gnss_cb = gnss_callback_;
		}
		if (gnss_cb) {
			gnss_cb(gnss);
		}
		MonitorGNSSStatus(gnss);
	}
}


void INSDeviceReceiver::HandleINSSolutionPacket(const uint8_t *packet) {
	constexpr size_t crc_offset = ACEINNA_HEADER_LEN + INS_SOLUTION_PACKET_LENGTH;
	const uint16_t recv_crc = packet[crc_offset] | (packet[crc_offset + 1] << 8);
	const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + INS_SOLUTION_PACKET_LENGTH);
	if (recv_crc != calc_crc) {
		stat_ins_crc_errors_.fetch_add(1, std::memory_order_relaxed);
		g_log.warn("INS solution data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}", recv_crc, calc_crc);
		return;
	}

	stat_ins_packets_.fetch_add(1, std::memory_order_relaxed);

	const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
	if (ins_bin_file_.IsOpen()) {
		ins_bin_file_.Write(reinterpret_cast<const char *>(payload), static_cast<std::streamsize>(INS_SOLUTION_PACKET_LENGTH));
	}
}


void INSDeviceReceiver::HandleDiagnosticPacket(const uint8_t *packet) {
	constexpr size_t crc_offset = ACEINNA_HEADER_LEN + DIAGNOSTIC_MESSAGE_LENGTH;
	const uint16_t recv_crc = packet[crc_offset] | (packet[crc_offset + 1] << 8);
	const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + DIAGNOSTIC_MESSAGE_LENGTH);
	if (recv_crc != calc_crc) {
		stat_diagnostic_crc_errors_.fetch_add(1, std::memory_order_relaxed);
		g_log.warn("Diagnostic message CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}", recv_crc, calc_crc);
		return;
	}

	stat_diagnostic_packets_.fetch_add(1, std::memory_order_relaxed);

	const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
	if (diagnostic_bin_file_.IsOpen()) {
		diagnostic_bin_file_.Write(reinterpret_cast<const char *>(payload), static_cast<std::streamsize>(DIAGNOSTIC_MESSAGE_LENGTH));
	}
}


void INSDeviceReceiver::HandleRawIMUPacket(const uint8_t *packet) {
	constexpr size_t crc_offset = ACEINNA_HEADER_LEN + RAW_IMU_DATA_LENGTH;
	const uint16_t recv_crc = packet[crc_offset] | (packet[crc_offset + 1] << 8);
	const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + RAW_IMU_DATA_LENGTH);
	if (recv_crc != calc_crc) {
		stat_imu_crc_errors_.fetch_add(1, std::memory_order_relaxed);
		g_log.warn("Raw IMU data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}", recv_crc, calc_crc);
		return;
	}

	stat_imu_packets_.fetch_add(1, std::memory_order_relaxed);

	const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
	if (imu_bin_file_.IsOpen()) {
		imu_bin_file_.Write(reinterpret_cast<const char *>(payload), static_cast<std::streamsize>(RAW_IMU_DATA_LENGTH));
	}

	RawIMUData imu = ParseRawIMUPayload(payload);
	ImuCallback imu_cb;
	{
		std::scoped_lock lock(callback_mutex_);
		imu_cb = imu_callback_;
	}
	if (imu_cb) {
		imu_cb(imu);
	}
}


void INSDeviceReceiver::HandleRTCMRoverPacket(const uint8_t *packet, size_t len) {
	size_t crc_offset = ACEINNA_HEADER_LEN + len;
	const uint16_t recv_crc = packet[crc_offset] | (packet[crc_offset + 1] << 8);
	const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + len);
	if (recv_crc != calc_crc) {
		stat_rtcm_rover_crc_errors_.fetch_add(1, std::memory_order_relaxed);
		g_log.warn("RTCM rover data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}", recv_crc, calc_crc);
		return;
	}

	stat_rtcm_rover_packets_.fetch_add(1, std::memory_order_relaxed);

	const uint8_t *rtcm_data = &packet[ACEINNA_HEADER_LEN];
	if (rtcm_rover_file_.IsOpen()) {
		rtcm_rover_file_.Write(reinterpret_cast<const char *>(rtcm_data), static_cast<std::streamsize>(len));
	}
}


void INSDeviceReceiver::HandleNMEAMessage(const uint8_t *packet, std::size_t max_len) {
	std::string nmea_msg(reinterpret_cast<const char *>(packet), max_len);
	const size_t end_pos = nmea_msg.find("\r\n");
	if (end_pos != std::string::npos) {
		nmea_msg = nmea_msg.substr(0, end_pos + 2);
		const size_t asterisk_pos = nmea_msg.find('*');
		if (asterisk_pos != std::string::npos && asterisk_pos + 2 < nmea_msg.size()) {
			// NMEA checksum: XOR of all bytes between '$' (exclusive) and '*' (exclusive)
			uint8_t checksum = 0;
			for (size_t i = 1; i < asterisk_pos; ++i) {
				checksum ^= static_cast<uint8_t>(nmea_msg[i]);
			}
			const std::string checksum_str = nmea_msg.substr(asterisk_pos + 1, 2);
			const auto recv_checksum = static_cast<uint8_t>(std::stoul(checksum_str, nullptr, 16));
			if (checksum != recv_checksum) {
				stat_nmea_checksum_errors_.fetch_add(1, std::memory_order_relaxed);
				g_log.warn("NMEA message checksum mismatch! Received: 0x{:02x} Calculated: 0x{:02x}", recv_checksum, checksum);
				return;
			}
		} else {
			stat_nmea_checksum_errors_.fetch_add(1, std::memory_order_relaxed);
			g_log.warn("NMEA message missing checksum!");
			return;
		}

		stat_nmea_messages_.fetch_add(1, std::memory_order_relaxed);

		if (nmea_file_.IsOpen()) {
			nmea_file_.Write(nmea_msg.data(), nmea_msg.size());
		}

		if (use_vrs_ || !first_gga_blh_ready_.load(std::memory_order_acquire)) {
			HandleGgaMessage(nmea_msg);
		}
	}
}


bool INSDeviceReceiver::IsGgaSentence(const std::string &nmea) {
	return nmea.rfind("$GPGGA,", 0) == 0 || nmea.rfind("$GNGGA,", 0) == 0;
}


void INSDeviceReceiver::HandleGgaMessage(const std::string &nmea) {
	if (!IsGgaSentence(nmea)) {
		return;
	}
	if (!first_gga_blh_ready_.load(std::memory_order_acquire)) {
		if (auto blh = ParseGgaCoordinates(nmea)) {
			first_gga_blh_ = *blh;
			first_gga_blh_ready_.store(true, std::memory_order_release);
			if (auto *initializer = initialization_monitor_.load(std::memory_order_acquire)) {
				initializer->SetBlhFromGga(*blh);
			}
		}
	}
	if (use_vrs_) {
		if (auto *client = ntrip_client_.load(std::memory_order_acquire)) {
			client->SetNmeaGga(nmea);
		}
	}
}


// GGA format: $G?GGA,time,lat,N/S,lon,E/W,qual,nsat,hdop,alt,M,geoid,M,age,stn*cs
// Latitude is ddmm.mmmm, longitude is dddmm.mmmm.
// Returns BLH in (radians, radians, meters) for Tool::Earth::ComputeGravity.
std::optional<Eigen::Vector3d> INSDeviceReceiver::ParseGgaCoordinates(const std::string &gga) {
	std::string sentence = gga;
	if (auto pos = sentence.find('*'); pos != std::string::npos) {
		sentence = sentence.substr(0, pos);
	}

	auto fields = Tool::Utility::SplitString(sentence, ',');
	if (fields.size() < 10) {
		return std::nullopt;
	}

	// Quality indicator: 0 = no fix
	if (fields[6].empty()) {
		return std::nullopt;
	}
	try {
		if (std::stoi(fields[6]) == 0) {
			return std::nullopt;
		}
	} catch (...) {
		return std::nullopt;
	}

	if (fields[2].empty() || fields[3].empty() || fields[4].empty() || fields[5].empty()) {
		return std::nullopt;
	}

	try {
		// Latitude: ddmm.mmmm -> decimal degrees -> radians
		double lat_raw = std::stod(fields[2]);
		double lat_deg = std::floor(lat_raw / 100.0);
		double latitude = lat_deg + (lat_raw - lat_deg * 100.0) / 60.0;
		if (fields[3] == "S") {
			latitude = -latitude;
		}

		// Longitude: dddmm.mmmm -> decimal degrees -> radians
		double lon_raw = std::stod(fields[4]);
		double lon_deg = std::floor(lon_raw / 100.0);
		double longitude = lon_deg + (lon_raw - lon_deg * 100.0) / 60.0;
		if (fields[5] == "W") {
			longitude = -longitude;
		}

		// MSL altitude + geoid separation = ellipsoidal height
		double altitude = fields[9].empty() ? 0.0 : std::stod(fields[9]);
		double geoid_sep = (fields.size() > 11 && !fields[11].empty()) ? std::stod(fields[11]) : 0.0;

		return Eigen::Vector3d(latitude * M_PI / 180.0, longitude * M_PI / 180.0, altitude + geoid_sep);
	} catch (...) {
		g_log.warn("Failed to parse numeric fields in GGA sentence");
		return std::nullopt;
	}
}


void INSDeviceReceiver::CloseAllFiles() {
	gnss_bin_file_.Close();
	ins_bin_file_.Close();
	imu_bin_file_.Close();
	diagnostic_bin_file_.Close();
	rtcm_rover_file_.Close();
	nmea_file_.Close();
}


bool INSDeviceReceiver::InitializeWritingFiles() {
	if (output_folder_path_.empty()) {
		Common::Log::log_and_throw(kModule, "Error: Output folder path is empty!");
		return false;
	}

	std::string binary_folder_path = fmt::format("{}/bin/ins401", output_folder_path_);
	std::filesystem::create_directories(binary_folder_path);

	gnss_bin_path_ = fmt::format("{}/gnss_{}.bin", binary_folder_path, timestamp_);
	ins_bin_path_ = fmt::format("{}/ins_{}.bin", binary_folder_path, timestamp_);
	imu_bin_path_ = fmt::format("{}/imu_{}.bin", binary_folder_path, timestamp_);
	diagnostic_bin_path_ = fmt::format("{}/diagnostic_{}.bin", binary_folder_path, timestamp_);

	std::string rtcm_rover_filename = fmt::format("{}/rtcm_rover_{}.rtcm3", binary_folder_path, timestamp_);
	std::string nmea_filename = fmt::format("{}/nmea_{}.nmea", binary_folder_path, timestamp_);

	// Every stream verifies its open: a silently unopened ofstream would
	// swallow the whole recording session
	const struct {
		Common::BufferedFileWriter *writer;
		const std::string *path;
		std::ios::openmode mode;
	} streams[] = {
		{ &gnss_bin_file_, &gnss_bin_path_, std::ios::out | std::ios::binary },
		{ &ins_bin_file_, &ins_bin_path_, std::ios::out | std::ios::binary },
		{ &imu_bin_file_, &imu_bin_path_, std::ios::out | std::ios::binary },
		{ &diagnostic_bin_file_, &diagnostic_bin_path_, std::ios::out | std::ios::binary },
		{ &rtcm_rover_file_, &rtcm_rover_filename, std::ios::out | std::ios::binary },
		{ &nmea_file_, &nmea_filename, std::ios::out },
	};
	for (const auto &st: streams) {
		if (!st.writer->Open(*st.path, write_buffer_size_, st.mode)) {
			g_log.error("Failed to open recording file: {}", *st.path);
			CloseAllFiles();
			return false;
		}
	}

	return true;
}


INSDeviceReceiver::Statistics INSDeviceReceiver::GetStatistics() const {
	Statistics s;
	s.gnss_packets = stat_gnss_packets_.load(std::memory_order_relaxed);
	s.ins_packets = stat_ins_packets_.load(std::memory_order_relaxed);
	s.imu_packets = stat_imu_packets_.load(std::memory_order_relaxed);
	s.diagnostic_packets = stat_diagnostic_packets_.load(std::memory_order_relaxed);
	s.rtcm_rover_packets = stat_rtcm_rover_packets_.load(std::memory_order_relaxed);
	s.nmea_messages = stat_nmea_messages_.load(std::memory_order_relaxed);
	s.gnss_crc_errors = stat_gnss_crc_errors_.load(std::memory_order_relaxed);
	s.ins_crc_errors = stat_ins_crc_errors_.load(std::memory_order_relaxed);
	s.imu_crc_errors = stat_imu_crc_errors_.load(std::memory_order_relaxed);
	s.diagnostic_crc_errors = stat_diagnostic_crc_errors_.load(std::memory_order_relaxed);
	s.rtcm_rover_crc_errors = stat_rtcm_rover_crc_errors_.load(std::memory_order_relaxed);
	s.nmea_checksum_errors = stat_nmea_checksum_errors_.load(std::memory_order_relaxed);
	s.total_bytes_received = stat_total_bytes_received_.load(std::memory_order_relaxed);
	return s;
}


void INSDeviceReceiver::LogStatistics() const {
	const Statistics statistic_result = GetStatistics();

	g_log.info("=== INS401 RECEIVER STATISTICS === : Total bytes received: {}", statistic_result.total_bytes_received);
	g_log.info("=== INS401 RECEIVER STATISTICS === : Received GNSS packet: {}; Number of GNSS packet CRC errors {}",
			   statistic_result.gnss_packets, statistic_result.gnss_crc_errors);
	g_log.info("=== INS401 RECEIVER STATISTICS === : Received INS packet: {}; Number of INS packet CRC errors {}",
			   statistic_result.ins_packets, statistic_result.ins_crc_errors);
	g_log.info("=== INS401 RECEIVER STATISTICS === : Received IMU packet: {}; Number of IMU packet CRC errors {}",
			   statistic_result.imu_packets, statistic_result.imu_crc_errors);
	g_log.info("=== INS401 RECEIVER STATISTICS === : Received Diag packet: {}; Number of Diag packet CRC errors {}",
			   statistic_result.diagnostic_packets, statistic_result.diagnostic_crc_errors);
	g_log.info("=== INS401 RECEIVER STATISTICS === : Received RTCM Rover packet: {}; Number of RTCM Rover packet CRC errors {}",
			   statistic_result.rtcm_rover_packets, statistic_result.rtcm_rover_crc_errors);
	g_log.info("=== INS401 RECEIVER STATISTICS === : Received NMEA Rover packet: {}; Number of NMEA Rover packet CRC errors {}",
			   statistic_result.nmea_messages, statistic_result.nmea_checksum_errors);
}


// Hysteresis-based GNSS state machine: require GnssTransitionConfirmFrames_ consecutive
// frames of a new state before accepting the transition, to avoid flapping on noisy signals.
void INSDeviceReceiver::MonitorGNSSStatus(GNSSSolutionData &gnss) {
	const bool current_rtk_fixed = gnss.position_type == 4;
	const double current_std = (gnss.latitude_std + gnss.longitude_std) / 2.0;
	const bool current_std_converged = current_std <= rtk_horizontal_std_;

	if (!gnss_state_initialized_) {
		gnss_state_initialized_ = true;
		stable_rtk_fixed_ = current_rtk_fixed;
		stable_std_converged_ = current_std_converged;
		pending_rtk_count_ = 0;
		pending_std_count_ = 0;
	} else {
		if (current_rtk_fixed == stable_rtk_fixed_) {
			pending_rtk_count_ = 0;
		} else {
			if (pending_rtk_count_ == 0 || pending_rtk_fixed_ != current_rtk_fixed) {
				pending_rtk_fixed_ = current_rtk_fixed;
				pending_rtk_count_ = 1;
			} else {
				++pending_rtk_count_;
			}
			if (pending_rtk_count_ >= GnssTransitionConfirmFrames_) {
				stable_rtk_fixed_ = current_rtk_fixed;
				pending_rtk_count_ = 0;
				const auto level = stable_rtk_fixed_ ? spdlog::level::info : spdlog::level::warn;
				g_log.log(level, "{}", stable_rtk_fixed_ ? "Entered RTK_FIXED position type" : "Lost RTK_FIXED position type");
			}
		}

		if (current_std_converged == stable_std_converged_) {
			pending_std_count_ = 0;
		} else {
			if (pending_std_count_ == 0 || pending_std_converged_ != current_std_converged) {
				pending_std_converged_ = current_std_converged;
				pending_std_count_ = 1;
			} else {
				++pending_std_count_;
			}
			if (pending_std_count_ >= GnssTransitionConfirmFrames_) {
				stable_std_converged_ = current_std_converged;
				pending_std_count_ = 0;
				const auto level = stable_std_converged_ ? spdlog::level::info : spdlog::level::warn;
				g_log.log(level, "{}",
						  stable_std_converged_ ? fmt::format("Converged to {:.3f} m STD threshold", rtk_horizontal_std_)
												: fmt::format("Horizontal STD diverged above {:.3f} m threshold", rtk_horizontal_std_));
			}
		}
	}
}
}  // namespace INS401
