#include "ins_receiver.h"

#include <cstring>
#include <iostream>
#include <net/if.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>



INSDeviceReceiver::INSDeviceReceiver(const std::string &iface, const std::string &target_mac, const std::string &local_mac,
									 bool save_to_file) :
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
		InitializeFiles();
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
			std::cerr << "Error: epoll_wait failed: " << strerror(errno) << std::endl;
			break;
		}
		for (int i = 0; i < nfds; ++i) {
			if (events[i].data.fd != sock_fd_) {
				continue;
			}
			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				std::cerr << "Socket error on " << interface_name_ << std::endl;
				continue;
			}
			if (events[i].events & EPOLLIN) {
				ssize_t bytes_read;
				do {
					bytes_read = ::recv(sock_fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
					if (bytes_read > 0) {
						VerifyData(buffer.data(), bytes_read);
					}
				} while (bytes_read > 0 || bytes_read < 0 && errno == EINTR);
			}
		}
	}
}


void INSDeviceReceiver::VerifyData(const uint8_t *data, const size_t len) {
	if (len < ETH_HEADER_LEN) {
		return;
	}
	const uint8_t *packet = data + ETH_HEADER_LEN;
	// Check command start
	if (packet[0] != COMMAND_START_BYTES[0] || packet[1] != COMMAND_START_BYTES[1]) {
		return;
	}
	const uint16_t recv_msg_id = packet[2] | (packet[3] << 8);
	const uint32_t data_length = packet[4] | (packet[5] << 8) | (packet[6] << 16) | (packet[7] << 24);
	switch (recv_msg_id) {
		case GNSS_SOLUTION_PACKET_ID:
			if (data_length == GNSS_SOLUTION_PACKET_LENGTH) {
				ProcessGNSSSolutionData(packet, data_length);
			} else {
				std::cerr << "Invalid GNSS solution data length: " << data_length << ", expected: " << GNSS_SOLUTION_PACKET_LENGTH
						  << std::endl;
			}
		case RAW_IMU_DATA_PACKET_ID:
			if (data_length == RAW_IMU_DATA_PACKET_LENGTH) {
				ProcessRawIMUData(packet, data_length);
			} else {
				std::cerr << "Invalid raw IMU data length: " << data_length << ", expected: " << RAW_IMU_DATA_PACKET_LENGTH
						  << std::endl;
			}
		default:
			break;
	}
}


void INSDeviceReceiver::ProcessGNSSSolutionData(const uint8_t *packet, const uint32_t data_length) {
	// Check CRC
	constexpr size_t crc_offset = ACENINNA_HEADER_LEN + GNSS_SOLUTION_PACKET_LENGTH;
	const uint16_t recv_crc = (packet[crc_offset] << 8) | packet[crc_offset + 1];  // MSB-first
	if (const uint16_t calc_crc = Tool::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + data_length); recv_crc != calc_crc) {
		std::cerr << "GNSS solution data CRC mismatch! Received: 0x" << std::hex << recv_crc << " Calculated: 0x" << calc_crc
				  << std::dec << std::endl;
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
	}
	cv_.notify_one();
}


void INSDeviceReceiver::ProcessRawIMUData(const uint8_t *packet, const uint32_t data_length) {
	// Check CRC
	constexpr size_t crc_offset = ACENINNA_HEADER_LEN + RAW_IMU_DATA_PACKET_LENGTH;
	const uint16_t recv_crc = (packet[crc_offset] << 8) | packet[crc_offset + 1];  // MSB-first
	if (const uint16_t calc_crc = Tool::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + data_length); recv_crc != calc_crc) {
		std::cerr << "Raw IMU data CRC mismatch! Received: 0x" << std::hex << recv_crc << " Calculated: 0x" << calc_crc << std::dec
				  << std::endl;
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


void INSDeviceReceiver::WriterThread() {
	std::vector<RawIMUData> imu_batch;
	std::vector<GNSSSolutionData> gnss_batch;
	imu_batch.reserve(imu_batch_size_);
	gnss_batch.reserve(gnss_batch_size_);

	std::string imu_buffer;
	std::string gnss_buffer;
	imu_buffer.reserve(imu_batch_size_ * 128);
	gnss_buffer.reserve(gnss_batch_size_ * 256);

	auto last_flush = std::chrono::steady_clock::now();
	constexpr auto FLUSH_INTERVAL = std::chrono::seconds(1);
	while (running_.load()) {
		{
			std::unique_lock lock(queue_mutex_);
			cv_.wait_for(lock, std::chrono::milliseconds(50),
						 [this] { return !imu_queue_.empty() || !gnss_queue_.empty() || !running_.load(); });
			imu_batch.clear();
			size_t imu_to_take = std::min(imu_batch_size_, imu_queue_.size());
			for (size_t i = 0; i < imu_to_take; ++i) {
				imu_batch.emplace_back(imu_queue_.front());
				imu_queue_.pop();
			}
			gnss_batch.clear();
			size_t gnss_to_take = std::min(gnss_batch_size_, gnss_queue_.size());
			for (size_t i = 0; i < gnss_to_take; ++i) {
				gnss_batch.emplace_back(gnss_queue_.front());
				gnss_queue_.pop();
			}

			if (gnss_queue_.size() > max_gnss_queue_size_) {
				const size_t to_remove = gnss_queue_.size() - max_gnss_queue_size_ / 2;
				for (size_t i = 0; i < to_remove; ++i) {
					gnss_queue_.pop();
				}
			}
			if (imu_queue_.size() > max_imu_queue_size_) {
				const size_t to_remove = imu_queue_.size() - max_imu_queue_size_ / 2;
				for (size_t i = 0; i < to_remove; ++i) {
					imu_queue_.pop();
				}
			}
		}
		if (save_to_file_) {
			WriteIMUBatch(imu_batch, imu_buffer);
			WriteGNSSBatch(gnss_batch, gnss_buffer);
			auto now = std::chrono::steady_clock::now();
			if (now - last_flush >= FLUSH_INTERVAL) {
				if (imu_file_.is_open())
					imu_file_.flush();
				if (gnss_file_.is_open())
					gnss_file_.flush();
				last_flush = now;
			}
		}
	}
	if (save_to_file_) {
		if (imu_file_.is_open()) {
			imu_file_.flush();
		}
		if (gnss_file_.is_open()) {
			gnss_file_.flush();
		}
	}
}


void INSDeviceReceiver::WriteIMUBatch(const std::vector<RawIMUData> &batch, std::string &buffer) {
	if (!imu_file_.is_open() || batch.empty()) {
		return;
	}
	buffer.clear();
	char line[256];
	for (const auto &imu: batch) {
		int len = snprintf(line, sizeof(line), "%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", imu.gps_week, imu.gps_millisecs, imu.acc_x,
						   imu.acc_y, imu.acc_z, imu.gyro_x, imu.gyro_y, imu.gyro_z);
		if (len > 0 && len < sizeof(line)) {
			buffer.append(line, len);
		}
	}
	imu_file_.write(buffer.data(), buffer.size());
}


void INSDeviceReceiver::WriteGNSSBatch(const std::vector<GNSSSolutionData> &batch, std::string &buffer) {
	if (!gnss_file_.is_open() || batch.empty()) {
		return;
	}
	buffer.clear();
	char line[512];
	for (const auto &gnss: batch) {
		int len = snprintf(line, sizeof(line),
						   "%u,%u,%d,%.9f,%.9f,%.3f,%.3f,%.3f,%.3f,%d,%d,"
						   "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
						   gnss.gps_week, gnss.gps_millisecs, static_cast<int>(gnss.position_type), gnss.latitude, gnss.longitude,
						   gnss.height, gnss.latitude_std, gnss.longitude_std, gnss.height_std, static_cast<int>(gnss.num_of_SVs),
						   static_cast<int>(gnss.num_of_SVs_in_solution), gnss.hdop, gnss.diffage, gnss.north_vel, gnss.east_vel,
						   gnss.up_vel, gnss.north_vel_std, gnss.east_vel_std, gnss.up_vel_std);
		if (len > 0 && len < sizeof(line)) {
			buffer.append(line, len);
		}
	}
	gnss_file_.write(buffer.data(), buffer.size());
}


void INSDeviceReceiver::InitializeFiles() {
	if (save_to_file_) {
		auto now = std::chrono::system_clock::now();
		auto time_t = std::chrono::system_clock::to_time_t(now);
		char timestamp[32];
		std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", std::localtime(&time_t));

		std::string gnss_filename = std::string("gnss_data_") + timestamp + ".txt";
		std::string imu_filename = std::string("imu_data_") + timestamp + ".txt";

		gnss_file_buffer_.resize(write_buffer_size_);
		gnss_file_.open(gnss_filename, std::ios::out);
		if (gnss_file_.is_open()) {
			gnss_file_.rdbuf()->pubsetbuf(gnss_file_buffer_.data(), gnss_file_buffer_.size());
			gnss_file_ << "GPS_Week,GPS_MS,Position_Type,Latitude,Longitude,Height,"
					   << "Latitude_STD,Longitude_STD,Height_STD,Num_of_SVs,"
					   << "Num_of_SVs_in_Solution,Hdop,Diffage,North_Vel,East_Vel,"
					   << "Up_Vel,North_Vel_STD,East_Vel_STD,Up_Vel_STD\n";
		}
		imu_file_buffer_.resize(write_buffer_size_);
		imu_file_.open(imu_filename, std::ios::out);
		if (imu_file_.is_open()) {
			gnss_file_.rdbuf()->pubsetbuf(imu_file_buffer_.data(), imu_file_buffer_.size());
			imu_file_ << "GPS_Week,GPS_MS,Acc_X,Acc_Y,Acc_Z,Gyro_X,Gyro_Y,Gyro_Z\n";
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
