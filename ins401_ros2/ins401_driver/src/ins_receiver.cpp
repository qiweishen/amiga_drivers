#include "ins_receiver.h"

#include <bitset>
#include <cstring>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

#include "fmt/xchar.h"



INSDeviceReceiver::INSDeviceReceiver(const std::string &iface, const std::string &target_mac, const std::string &local_mac,
									 const bool save_to_file) :
	sock_fd_(-1), interface_name_(iface), running_(true), save_to_file_(save_to_file) {
	Tool::Ethernet::ParseMACAddressToUint8(target_mac, target_mac_);
	Tool::Ethernet::ParseMACAddressToUint8(local_mac, local_mac_);
}


INSDeviceReceiver::~INSDeviceReceiver() {
	Stop();
	cv_.notify_all();
	if (writer_thread_.joinable()) {
		writer_thread_.join();
	}
	if (sock_fd_ >= 0) {
		close(sock_fd_);
	}
	if (imu_file_.is_open()) {
		imu_file_.close();
	}
}


void INSDeviceReceiver::Run() {
	if (save_to_file_) {
		InitializeWritingFiles();
	}
	Initialize();
	ReceiveLoop();
}


void INSDeviceReceiver::Stop() {
	running_ = false;
}


bool INSDeviceReceiver::GetGNSSData(std::vector<GNSSSolutionData> &data, size_t max_count) {
	std::lock_guard lock(queue_mutex_);
	if (gnss_queue_.empty()) {
		return false;
	}
	size_t count = 0;
	while (!gnss_queue_.empty() && count < max_count) {
		data.push_back(gnss_queue_.front());
		gnss_queue_.pop();
		count++;
	}
	return true;
}


bool INSDeviceReceiver::GetIMUData(std::vector<RawIMUData> &data, size_t max_count) {
	std::lock_guard lock(queue_mutex_);
	if (imu_queue_.empty()) {
		return false;
	}
	size_t count = 0;
	while (!imu_queue_.empty() && count < max_count) {
		data.push_back(imu_queue_.front());
		imu_queue_.pop();
		count++;
	}
	return true;
}


bool INSDeviceReceiver::Initialize() {
	if (!Tool::Ethernet::CreateAsyncRawSocket(sock_fd_, interface_name_, target_mac_, local_mac_)) {
		return false;
	}
	return true;
}


void INSDeviceReceiver::ReceiveLoop() {
	int epfd = -1;
	if (!Tool::Ethernet::SetupEpollForFd(sock_fd_, epfd, EPOLLIN | EPOLLET)) {
		return;
	}
	Tool::Ethernet::EpollGuard epoll_guard(epfd);

	// Estimate max size of single packet
	std::vector<uint8_t> buffer(buffer_size_);
	constexpr int MAX_EVENTS = 4;
	epoll_event events[MAX_EVENTS];

	while (running_.load()) {
		const int nfds = ::epoll_wait(epfd, events, MAX_EVENTS, 100);
		if (nfds < 0) {
			if (errno == EINTR) {
				continue;
			}
			fmt::print(stderr, "[INS401 Receiver] Error: epoll_wait failed: {}\n", strerror(errno));
			break;
		}
		for (int i = 0; i < nfds; ++i) {
			if (events[i].data.fd != sock_fd_) {
				continue;
			}
			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				fmt::print(stderr, "[INS401 Receiver] Socket error on {}\n", interface_name_);
				continue;
			}
			if (events[i].events & EPOLLIN) {
				ssize_t bytes_read;
				do {
					bytes_read = ::recv(sock_fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
					if (bytes_read > 0) {
						VerifyDataFrame(buffer.data(), bytes_read);
					}
				} while (bytes_read > 0 || bytes_read < 0 && errno == EINTR);
			}
		}
	}
}


void INSDeviceReceiver::VerifyDataFrame(const uint8_t *data, const size_t len) {
	if (len < ETH_HEADER_LEN) {
		return;
	}
	const uint8_t *packet = data + ETH_HEADER_LEN;
	// Check Aceinna binary command start
	if (packet[0] == COMMAND_START_BYTES[0] && packet[1] == COMMAND_START_BYTES[1]) {
		const uint16_t recv_msg_id = packet[2] | (packet[3] << 8);
		const uint32_t data_length = packet[4] | (packet[5] << 8) | (packet[6] << 16) | (packet[7] << 24);
		switch (recv_msg_id) {
			case GNSS_SOLUTION_PACKET_MESSAGE_ID:
				if (data_length == GNSS_SOLUTION_PACKET_LENGTH) {
					ProcessGNSSSolutionData(packet);
				} else {
					fmt::print(stderr, "[INS401 Receiver] Invalid GNSS solution data length: {}, expected: {}\n", data_length,
							   GNSS_SOLUTION_PACKET_LENGTH);
				}
				break;
			case DIAGNOSTIC_MESSAGE_ID:
				if (data_length == DIAGNOSTIC_MESSAGE_LENGTH) {
					ProcessDiagnosticMessage(packet);
				} else {
					fmt::print(stderr, "[INS401 Receiver] Invalid diagnostic message length: {}, expected: {}\n", data_length,
							   DIAGNOSTIC_MESSAGE_LENGTH);
				}
				break;
			case RAW_IMU_DATA_MESSAGE_ID:
				if (data_length == RAW_IMU_DATA_LENGTH) {
					ProcessRawIMUData(packet);
				} else {
					fmt::print(stderr, "[INS401 Receiver] Invalid raw IMU data length: {}, expected: {}\n", data_length,
							   RAW_IMU_DATA_LENGTH);
				}
				break;
			case RTCM_ROVER_DATA_MESSAGE_ID:
				if (data_length >= 1 && data_length <= RTCM_ROVER_DATA_LENGTH_MAX) {
					ProcessRTCMRoverData(packet, data_length);
				} else {
					fmt::print(stderr, "[INS401 Receiver] Invalid RTCM rover data length: {}, expected: 1-{}\n", data_length,
							   RTCM_ROVER_DATA_LENGTH_MAX);
				}
				break;
			default:
				break;
		}
	} else if (packet[0] == NEMA_ASCII_START) {	 // Check NEMA ASCII start
		ProcessNMEAMessage(packet);
	} else {
		// Unknown packet type
		return;
	}
}


void INSDeviceReceiver::ProcessGNSSSolutionData(const uint8_t *packet) {
	// Check CRC
	constexpr size_t crc_offset = ACENINNA_HEADER_LEN + GNSS_SOLUTION_PACKET_LENGTH;
	const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8;  // LSB-first
	const uint16_t calc_crc = Tool::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + GNSS_SOLUTION_PACKET_LENGTH);
	if (recv_crc != calc_crc) {
		fmt::print(stderr, "[INS401 Receiver] GNSS solution data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}\n", recv_crc,
				   calc_crc);
		return;
	}
	// Parse GNSS solution data
	const uint8_t *gnss_data = &packet[ACENINNA_HEADER_LEN];
	GNSSSolutionData gnss{};
	gnss.gps_week = *reinterpret_cast<const uint16_t *>(gnss_data);
	gnss.gps_millisecs = *reinterpret_cast<const uint32_t *>(gnss_data + 2);
	gnss.position_type = gnss_data[6];
	std::memcpy(&gnss.latitude, gnss_data + 7, sizeof(double));
	std::memcpy(&gnss.longitude, gnss_data + 15, sizeof(double));
	std::memcpy(&gnss.height, gnss_data + 23, sizeof(double));
	std::memcpy(&gnss.latitude_std, gnss_data + 31, sizeof(float));
	std::memcpy(&gnss.longitude_std, gnss_data + 35, sizeof(float));
	std::memcpy(&gnss.height_std, gnss_data + 39, sizeof(float));
	gnss.num_of_SVs = gnss_data[43];
	gnss.num_of_SVs_in_solution = gnss_data[44];
	std::memcpy(&gnss.hdop, gnss_data + 45, sizeof(float));
	std::memcpy(&gnss.diffage, gnss_data + 49, sizeof(float));
	std::memcpy(&gnss.north_vel, gnss_data + 53, sizeof(float));
	std::memcpy(&gnss.east_vel, gnss_data + 57, sizeof(float));
	std::memcpy(&gnss.up_vel, gnss_data + 61, sizeof(float));
	std::memcpy(&gnss.north_vel_std, gnss_data + 65, sizeof(float));
	std::memcpy(&gnss.east_vel_std, gnss_data + 69, sizeof(float));
	std::memcpy(&gnss.up_vel_std, gnss_data + 73, sizeof(float));
	{
		std::lock_guard lock(queue_mutex_);
		if (gnss_queue_.size() >= max_gnss_queue_size_) {
			gnss_queue_.pop();
		}
		gnss_queue_.push(gnss);
		fmt::print(stdout, "[INS401 Receiver] GNSS Solution: Week {}, Time {} ms, Position Type {}, STD: {:.6f}, {:.6f}, {:.6f}\n",
				   gnss.gps_week, gnss.gps_millisecs, gnss.position_type, gnss.latitude_std, gnss.longitude_std, gnss.height_std);
	}
	cv_.notify_one();
}


void INSDeviceReceiver::ProcessDiagnosticMessage(const uint8_t *packet) {
	// Check CRC
	constexpr size_t crc_offset = ACENINNA_HEADER_LEN + DIAGNOSTIC_MESSAGE_LENGTH;
	const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8;  // LSB-first
	const uint16_t calc_crc = Tool::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + DIAGNOSTIC_MESSAGE_LENGTH);
	if (recv_crc != calc_crc) {
		fmt::print(stderr, "[INS401 Receiver] Diagnostic message CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}\n", recv_crc,
				   calc_crc);
		return;
	}
	// Parse Diagnostic message
	const uint8_t *diagnostic_messages = &packet[ACENINNA_HEADER_LEN];
	DiagnosticMessage diagnostic_msg{};
	diagnostic_msg.gps_week = *reinterpret_cast<const uint16_t *>(diagnostic_messages);
	diagnostic_msg.gps_millisecs = *reinterpret_cast<const uint32_t *>(diagnostic_messages + 2);
	// Device status - 32 bytes
	uint32_t status_value;
	std::memcpy(&status_value, diagnostic_messages + 6, sizeof(uint32_t));
	const std::bitset<32> bs(status_value);
	for (int i = 0; i < 32; ++i) {
		diagnostic_msg.device_status[i] = bs[i];
	}
	std::memcpy(&diagnostic_msg.imu_temperature, diagnostic_messages + 10, sizeof(float));
	std::memcpy(&diagnostic_msg.mcu_temperature, diagnostic_messages + 14, sizeof(float));
	std::memcpy(&diagnostic_msg.gnss_chip_temperature, diagnostic_messages + 18, sizeof(float));
	{
		std::lock_guard lock(queue_mutex_);
		if (diagnostic_queue_.size() >= max_diagnostic_queue_size_) {
			diagnostic_queue_.pop();
		}
		diagnostic_queue_.push(diagnostic_msg);
	}
	cv_.notify_one();
}


void INSDeviceReceiver::ProcessRawIMUData(const uint8_t *packet) {
	// Check CRC
	constexpr size_t crc_offset = ACENINNA_HEADER_LEN + RAW_IMU_DATA_LENGTH;
	const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8;  // LSB-first
	const uint16_t calc_crc = Tool::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + RAW_IMU_DATA_LENGTH);
	if (recv_crc != calc_crc) {
		fmt::print(stderr, "[INS401 Receiver] Raw IMU data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}\n", recv_crc,
				   calc_crc);
		return;
	}
	// Parse IMU data
	const uint8_t *imu_data = &packet[ACENINNA_HEADER_LEN];
	RawIMUData imu{};
	imu.gps_week = *reinterpret_cast<const uint16_t *>(imu_data);
	imu.gps_millisecs = *reinterpret_cast<const uint32_t *>(imu_data + 2);
	std::memcpy(&imu.acc_x, imu_data + 6, sizeof(float));
	std::memcpy(&imu.acc_y, imu_data + 10, sizeof(float));
	std::memcpy(&imu.acc_z, imu_data + 14, sizeof(float));
	std::memcpy(&imu.gyro_x, imu_data + 18, sizeof(float));
	std::memcpy(&imu.gyro_y, imu_data + 22, sizeof(float));
	std::memcpy(&imu.gyro_z, imu_data + 26, sizeof(float));
	{
		std::lock_guard lock(queue_mutex_);
		if (imu_queue_.size() >= max_imu_queue_size_) {
			imu_queue_.pop();
		}
		imu_queue_.push(imu);
	}
	cv_.notify_one();
}


void INSDeviceReceiver::ProcessRTCMRoverData(const uint8_t *packet, size_t len) {
	// Check CRC
	size_t crc_offset = ACENINNA_HEADER_LEN + len;
	const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8;  // LSB-first
	const uint16_t calc_crc = Tool::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + len);
	if (recv_crc != calc_crc) {
		fmt::print(stderr, "[INS401 Receiver] RTCM rover data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}\n", recv_crc,
				   calc_crc);
		return;
	}
	// Parse RTCM data
	const uint8_t *rtcm_data = &packet[ACENINNA_HEADER_LEN];
	std::vector<uint8_t> rtcm_vector(rtcm_data, rtcm_data + len);
	{
		std::lock_guard lock(queue_mutex_);
		if (rtcm_rover_queue_.size() >= max_rtcm_rover_queue_size_) {
			rtcm_rover_queue_.pop();
		}
		rtcm_rover_queue_.push(rtcm_vector);
	}
	cv_.notify_one();
}



void INSDeviceReceiver::ProcessNMEAMessage(const uint8_t *packet) {
	std::string nmea_msg(reinterpret_cast<const char *>(packet));
	// Find the end of the NMEA message
	const size_t end_pos = nmea_msg.find("\r\n");
	if (end_pos != std::string::npos) {
		nmea_msg = nmea_msg.substr(0, end_pos + 2);
		// Check NMEA checksum
		const size_t asterisk_pos = nmea_msg.find('*');
		if (asterisk_pos != std::string::npos && asterisk_pos + 2 < nmea_msg.size()) {
			uint8_t checksum = 0;
			for (size_t i = 1; i < asterisk_pos; ++i) {
				checksum ^= static_cast<uint8_t>(nmea_msg[i]);
			}
			const std::string checksum_str = nmea_msg.substr(asterisk_pos + 1, 2);
			const auto recv_checksum = static_cast<uint8_t>(std::stoul(checksum_str, nullptr, 16));
			if (checksum != recv_checksum) {
				fmt::print(stderr, "[INS401 Receiver] NMEA message checksum mismatch! Received: 0x{:02x} Calculated: 0x{:02x}\n",
						   recv_checksum, checksum);
				return;
			}
		} else {
			fmt::print(stderr, "[INS401 Receiver] NMEA message missing checksum!\n");
			return;
		}
		{
			std::lock_guard lock(queue_mutex_);
			if (nmea_queue_.size() >= max_nmea_queue_size_) {
				nmea_queue_.pop();
			}
			nmea_queue_.push(nmea_msg);
		}
		cv_.notify_one();
	}
}


void INSDeviceReceiver::WriterThread() {
	std::vector<GNSSSolutionData> gnss_batch;
	std::vector<DiagnosticMessage> diagnostic_batch;
	std::vector<RawIMUData> imu_batch;
	std::vector<std::vector<uint8_t>> rtcm_rover_batch;
	std::vector<std::string> nmea_batch;
	gnss_batch.reserve(gnss_write_batch_size_);
	diagnostic_batch.reserve(diagnostic_write_batch_size_);
	imu_batch.reserve(imu_write_batch_size_);
	rtcm_rover_batch.reserve(rtcm_rover_write_batch_size_);
	nmea_batch.reserve(nmea_write_batch_size_);

	auto last_flush = std::chrono::steady_clock::now();
	constexpr auto FLUSH_INTERVAL = std::chrono::seconds(1);
	while (running_.load()) {
		{
			std::unique_lock lock(queue_mutex_);
			cv_.wait_for(lock, std::chrono::milliseconds(50),
						 [this] { return !imu_queue_.empty() || !gnss_queue_.empty() || !running_.load(); });
			gnss_batch.clear();
			const size_t gnss_to_take = std::min(gnss_write_batch_size_, gnss_queue_.size());
			for (size_t i = 0; i < gnss_to_take; ++i) {
				gnss_batch.emplace_back(gnss_queue_.front());
				gnss_queue_.pop();
			}
			diagnostic_batch.clear();
			const size_t diagnostic_to_take = std::min(diagnostic_write_batch_size_, diagnostic_queue_.size());
			for (size_t i = 0; i < diagnostic_to_take; ++i) {
				diagnostic_batch.emplace_back(diagnostic_queue_.front());
				diagnostic_queue_.pop();
			}
			imu_batch.clear();
			const size_t imu_to_take = std::min(imu_write_batch_size_, imu_queue_.size());
			for (size_t i = 0; i < imu_to_take; ++i) {
				imu_batch.emplace_back(imu_queue_.front());
				imu_queue_.pop();
			}
			rtcm_rover_batch.clear();
			const size_t rtcm_rover_to_take = std::min(rtcm_rover_write_batch_size_, rtcm_rover_queue_.size());
			for (size_t i = 0; i < rtcm_rover_to_take; ++i) {
				rtcm_rover_batch.emplace_back(rtcm_rover_queue_.front());
				rtcm_rover_queue_.pop();
			}
			nmea_batch.clear();
			const size_t nmea_to_take = std::min(nmea_write_batch_size_, nmea_queue_.size());
			for (size_t i = 0; i < nmea_to_take; ++i) {
				nmea_batch.emplace_back(nmea_queue_.front());
				nmea_queue_.pop();
			}

			if (gnss_queue_.size() > max_gnss_queue_size_) {
				const size_t to_remove = gnss_queue_.size() - max_gnss_queue_size_ / 2;
				for (size_t i = 0; i < to_remove; ++i) {
					gnss_queue_.pop();
				}
			}
			if (diagnostic_queue_.size() > max_diagnostic_queue_size_) {
				const size_t to_remove = diagnostic_queue_.size() - max_diagnostic_queue_size_ / 2;
				for (size_t i = 0; i < to_remove; ++i) {
					diagnostic_queue_.pop();
				}
			}
			if (imu_queue_.size() > max_imu_queue_size_) {
				const size_t to_remove = imu_queue_.size() - max_imu_queue_size_ / 2;
				for (size_t i = 0; i < to_remove; ++i) {
					imu_queue_.pop();
				}
			}
			if (rtcm_rover_queue_.size() > max_rtcm_rover_queue_size_) {
				const size_t to_remove = rtcm_rover_queue_.size() - max_rtcm_rover_queue_size_ / 2;
				for (size_t i = 0; i < to_remove; ++i) {
					rtcm_rover_queue_.pop();
				}
			}
			if (nmea_queue_.size() > max_nmea_queue_size_) {
				const size_t to_remove = nmea_queue_.size() - max_nmea_queue_size_ / 2;
				for (size_t i = 0; i < to_remove; ++i) {
					nmea_queue_.pop();
				}
			}
		}
		if (save_to_file_) {
			WriteGNSSBatch(gnss_batch);
			WriteDiagnosticBatch(diagnostic_batch);
			WriteIMUBatch(imu_batch);
			WriteRTCMRoverBatch(rtcm_rover_batch);
			WriteNMEABatch(nmea_batch);
			auto now = std::chrono::steady_clock::now();
			if (now - last_flush >= FLUSH_INTERVAL) {
				if (gnss_file_.is_open()) {
					gnss_file_.flush();
				}
				if (diagnostic_file_.is_open()) {
					diagnostic_file_.flush();
				}
				if (imu_file_.is_open()) {
					imu_file_.flush();
				}
				if (rtcm_rover_file_.is_open()) {
					rtcm_rover_file_.flush();
				}
				if (nmea_file_.is_open()) {
					nmea_file_.flush();
				}
				last_flush = now;
			}
		}
	}
	if (save_to_file_) {
		if (gnss_file_.is_open()) {
			gnss_file_.flush();
		}
		if (diagnostic_file_.is_open()) {
			diagnostic_file_.flush();
		}
		if (imu_file_.is_open()) {
			imu_file_.flush();
		}
		if (rtcm_rover_file_.is_open()) {
			rtcm_rover_file_.flush();
		}
		if (nmea_file_.is_open()) {
			nmea_file_.flush();
		}
	}
}


void INSDeviceReceiver::WriteGNSSBatch(const std::vector<GNSSSolutionData> &batch) {
	if (!gnss_file_.is_open() || batch.empty()) {
		return;
	}
	fmt::memory_buffer buffer;
	buffer.reserve(batch.size() * 256);	 // Reserve approximate size
	for (const auto &gnss: batch) {
		fmt::format_to(std::back_inserter(buffer), "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", gnss.gps_week,
					   gnss.gps_millisecs, gnss.position_type, gnss.latitude, gnss.longitude, gnss.height, gnss.latitude_std,
					   gnss.longitude_std, gnss.height_std, gnss.num_of_SVs, gnss.num_of_SVs_in_solution, gnss.hdop, gnss.diffage,
					   gnss.north_vel, gnss.east_vel, gnss.up_vel, gnss.north_vel_std, gnss.east_vel_std, gnss.up_vel_std);
	}
	gnss_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}


void INSDeviceReceiver::WriteDiagnosticBatch(const std::vector<DiagnosticMessage> &batch) {
	if (!diagnostic_file_.is_open() || batch.empty()) {
		return;
	}
	fmt::memory_buffer buffer;
	buffer.reserve(batch.size() * 128);	 // Reserve approximate size
	for (const auto &diag: batch) {
		fmt::format_to(std::back_inserter(buffer), "{},{},{},{},{},{},{}\n", diag.gps_week, diag.gps_millisecs,
					   fmt::join(diag.device_status, ";"), diag.imu_temperature, diag.mcu_temperature, diag.gnss_chip_temperature);
	}
	diagnostic_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}



void INSDeviceReceiver::WriteIMUBatch(const std::vector<RawIMUData> &batch) {
	if (!imu_file_.is_open() || batch.empty()) {
		return;
	}
	fmt::memory_buffer buffer;
	buffer.reserve(batch.size() * 128);	 // Reserve approximate size
	for (const auto &imu: batch) {
		fmt::format_to(std::back_inserter(buffer), "{},{},{},{},{},{},{},{}\n", imu.gps_week, imu.gps_millisecs, imu.acc_x, imu.acc_y,
					   imu.acc_z, imu.gyro_x, imu.gyro_y, imu.gyro_z);
	}
	imu_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}


void INSDeviceReceiver::WriteRTCMRoverBatch(const std::vector<std::vector<uint8_t>> &batch) {
	if (!rtcm_rover_file_.is_open() || batch.empty()) {
		return;
	}
	for (const auto &rtcm: batch) {
		rtcm_rover_file_.write(reinterpret_cast<const char *>(rtcm.data()), static_cast<std::streamsize>(rtcm.size()));
	}
}


void INSDeviceReceiver::WriteNMEABatch(const std::vector<std::string> &batch) {
	if (!nmea_file_.is_open() || batch.empty()) {
		return;
	}
	fmt::memory_buffer buffer;
	buffer.reserve(batch.size() * 128);	 // Reserve approximate size
	for (const auto &nmea: batch) {
		fmt::format_to(std::back_inserter(buffer), "{}", nmea);
	}
	nmea_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}


void INSDeviceReceiver::InitializeWritingFiles() {
	if (save_to_file_) {
		auto now = std::chrono::system_clock::now();
		auto time_t = std::chrono::system_clock::to_time_t(now);
		std::string timestamp = fmt::format("{:%Y%m%d_%H%M%S}", *std::localtime(&time_t));

		std::filesystem::path data_dir = "./data";
		if (!std::filesystem::exists(data_dir)) {
			std::filesystem::create_directory(data_dir);
		}

		std::string gnss_filename = fmt::format("./data/gnss_data_{}.txt", timestamp);
		std::string diagnostic_filename = fmt::format("./data/diagnostic_data_{}.txt", timestamp);
		std::string imu_filename = fmt::format("./data/imu_data_{}.txt", timestamp);
		std::string rtcm_rover_filename = fmt::format("./data/rtcm_rover_data_{}.rtcm3", timestamp);
		std::string nmea_filename = fmt::format("./data/nmea_message_{}.txt", timestamp);

		gnss_file_buffer_.resize(write_buffer_size_);
		gnss_file_.open(gnss_filename, std::ios::out);
		if (gnss_file_.is_open()) {
			gnss_file_.rdbuf()->pubsetbuf(gnss_file_buffer_.data(), static_cast<std::streamsize>(gnss_file_buffer_.size()));
			fmt::print(gnss_file_,
					   "GPS_Week,GPS_MS,Position_Type,Latitude,Longitude,Height,Latitude_STD,Longitude_STD,Height_STD,Num_of_SVs,Num_"
					   "of_SVs_in_Solution,Hdop,Diffage,North_Vel,East_Vel,Up_Vel,North_Vel_STD,East_Vel_STD,Up_Vel_STD\n");
		}
		diagnostic_file_buffer_.resize(write_buffer_size_);
		diagnostic_file_.open(diagnostic_filename, std::ios::out);
		if (diagnostic_file_.is_open()) {
			diagnostic_file_.rdbuf()->pubsetbuf(diagnostic_file_buffer_.data(),
												static_cast<std::streamsize>(diagnostic_file_buffer_.size()));
			fmt::print(diagnostic_file_, "GPS_Week,GPS_MS,Device_Status,IMU_Temperature,MCU_Temperature,GNSS_Chip_Temperature\n");
		}
		imu_file_buffer_.resize(write_buffer_size_);
		imu_file_.open(imu_filename, std::ios::out);
		if (imu_file_.is_open()) {
			imu_file_.rdbuf()->pubsetbuf(imu_file_buffer_.data(), static_cast<std::streamsize>(imu_file_buffer_.size()));
			fmt::print(imu_file_, "GPS_Week,GPS_MS,Acc_X,Acc_Y,Acc_Z,Gyro_X,Gyro_Y,Gyro_Z\n");
		}
		rtcm_rover_file_buffer_.resize(write_buffer_size_);
		rtcm_rover_file_.open(rtcm_rover_filename, std::ios::out | std::ios::binary);
		if (rtcm_rover_file_.is_open()) {
			rtcm_rover_file_.rdbuf()->pubsetbuf(rtcm_rover_file_buffer_.data(),
												static_cast<std::streamsize>(rtcm_rover_file_buffer_.size()));
		}
		nmea_file_buffer_.resize(write_buffer_size_);
		nmea_file_.open(nmea_filename, std::ios::out);
		if (nmea_file_.is_open()) {
			nmea_file_.rdbuf()->pubsetbuf(nmea_file_buffer_.data(), static_cast<std::streamsize>(nmea_file_buffer_.size()));
		}

		writer_thread_ = std::thread(&INSDeviceReceiver::WriterThread, this);
	}
}
//
//
// void INSDeviceReceiver::WriteIMUBinary(const std::vector<RawIMUData>& batch) {
// 	if (!imu_binary_file_.is_open() || batch.empty()) return;
//
//
// 	imu_binary_file_.write(reinterpret_cast<const char*>(batch.data()),
// 						   batch.size() * sizeof(RawIMUData));
// }
