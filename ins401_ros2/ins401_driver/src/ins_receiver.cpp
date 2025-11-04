#include "ins_receiver.h"

#include <cstring>
#include <iostream>
#include <net/if.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>
#include <cstring>



INSDeviceReceiver::INSDeviceReceiver(const std::string &iface, const std::string &mac_addr, bool save_to_file)
	: sock_fd_(-1), COMMAND_START_(new uint8_t[2]), interface_name_(std::move(iface)),
	  running_(true), save_to_file_(save_to_file) {
	Tool::Ethernet::ConvertUint16ToUint8(COMMAND_START, COMMAND_START_, LSB);
	Tool::Ethernet::ParseMACAddressToUint8(mac_addr, target_mac_);
	if (save_to_file_) {
		gnss_file_.open("gnss_data.txt", std::ios::out | std::ios::app);
		if (gnss_file_.is_open()) {
			gnss_file_ << "GPS_Week,GPS_MS,Position_Type,Latitude,Longitude,Height,"
					<< "Latitude_STD,Longitude_STD,Height_STD,Num_of_SVs,"
					<< "Num_of_SVs_in_Solution,Hdop,Diffage,North_Vel,East_Vel,"
					<< "Up_Vel,North_Vel_STD,East_Vel_STD,Up_Vel_STD\n";
		}
		imu_file_.open("imu_data.txt", std::ios::out | std::ios::app);
		if (imu_file_.is_open()) {
			imu_file_ << "GPS_Week,GPS_MS,Acc_X,Acc_Y,Acc_Z,Gyro_X,Gyro_Y,Gyro_Z\n";
		}
		writer_thread_ = std::thread(&INSDeviceReceiver::WriterThread, this);
	}
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


void INSDeviceReceiver::Stop() {
	running_ = false;
}


void INSDeviceReceiver::Run() {
	Initialize();
	ReceiveLoop();
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


// void INSDeviceReceiver::HandleRTCMMessage(const uint8_t *data, size_t size) {
// 	if (size >= 6) {
// 		// Get the payload
// 		const uint8_t *payload = data + 3;
// 		const size_t payload_size = size - 3;
// 		// Send via raw socket
// 		if (sock_fd_ >= 0) {
// 			ssize_t sent_bytes = send(sock_fd_, payload, payload_size, 0);
// 			if (sent_bytes < 0) {
// 			}
// 		}
// 	}
// }


bool INSDeviceReceiver::Initialize() {
	if (!Tool::Ethernet::CreateAsyncRawSocket(sock_fd_, interface_name_)) {
		return false;
	}
	return true;
}


void INSDeviceReceiver::VerifyData(const uint8_t *data, const size_t len) {
	if (len < ETH_HEADER_LEN) {
		return;
	}
	const uint8_t *packet = data + ETH_HEADER_LEN;
	if (packet[0] == COMMAND_START_[0] && packet[1] == COMMAND_START_[1]) {
		const uint16_t recv_msg_id = packet[2] | (packet[3] << 8);
		if (recv_msg_id == RAW_IMU_DATA_PACKET_ID) {
			ProcessRawIMUData(packet);
		} else if (recv_msg_id == GNSS_SOLUTION_PACKET_ID) {
			ProcessGNSSSolutionData(packet);
		}
	}
}


void INSDeviceReceiver::ProcessGNSSSolutionData(const uint8_t *packet) {
	const uint32_t data_length = packet[4] |
	                             (packet[5] << 8) |
	                             (packet[6] << 16) |
	                             (packet[7] << 24);
	// Check length
	if (data_length != GNSS_SOLUTION_PACKET_LENGTH) {
		std::cerr << "Invalid GNSS solution data length: " << data_length
				<< ", expected: " << GNSS_SOLUTION_PACKET_LENGTH << std::endl;
		return;
	}
	// Check CRC
	constexpr size_t crc_offset = ACENINNA_HEADER_LEN + GNSS_SOLUTION_PACKET_LENGTH;
	const uint16_t recv_crc = (packet[crc_offset] << 8) | packet[crc_offset + 1]; // MSB-first
	if (const uint16_t calc_crc = Tool::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + data_length);
		recv_crc != calc_crc) {
		std::cerr << "GNSS solution data CRC mismatch! Received: 0x" << std::hex << recv_crc
				<< " Calculated: 0x" << calc_crc << std::dec << std::endl;
		return;
	}
	// Parse GNSS solution data
	const uint8_t *gnss_solution_data = &packet[ACENINNA_HEADER_LEN];
	GNSSSolutionData gnss{};
	gnss.gps_week = gnss_solution_data[0] | (gnss_solution_data[1] << 8);
	gnss.gps_millisecs = gnss_solution_data[2] | (gnss_solution_data[3] << 8) |
	                     (gnss_solution_data[4] << 16) | (gnss_solution_data[5] << 24);
	gnss.position_type = gnss_solution_data[6];
	std::memcpy(&gnss.latitude, &gnss_solution_data[7], sizeof(double));
	std::memcpy(&gnss.longitude, &gnss_solution_data[15], sizeof(double));
	std::memcpy(&gnss.height, &gnss_solution_data[23], sizeof(double));
	std::memcpy(&gnss.latitude_std, &gnss_solution_data[31], sizeof(float));
	std::memcpy(&gnss.longitude_std, &gnss_solution_data[35], sizeof(float));
	std::memcpy(&gnss.height_std, &gnss_solution_data[39], sizeof(float));
	gnss.num_of_SVs = gnss_solution_data[43];
	gnss.num_of_SVs_in_solution = gnss_solution_data[44];
	std::memcpy(&gnss.hdop, &gnss_solution_data[45], sizeof(float));
	std::memcpy(&gnss.diffage, &gnss_solution_data[49], sizeof(float));
	std::memcpy(&gnss.north_vel, &gnss_solution_data[53], sizeof(float));
	std::memcpy(&gnss.east_vel, &gnss_solution_data[57], sizeof(float));
	std::memcpy(&gnss.up_vel, &gnss_solution_data[61], sizeof(float));
	std::memcpy(&gnss.north_vel_std, &gnss_solution_data[65], sizeof(float));
	std::memcpy(&gnss.east_vel_std, &gnss_solution_data[69], sizeof(float));
	std::memcpy(&gnss.up_vel_std, &gnss_solution_data[73], sizeof(float)); {
		std::lock_guard<std::mutex> lock(queue_mutex_);
		gnss_queue_.push(gnss);
	}
	cv_.notify_one();
}


void INSDeviceReceiver::ProcessRawIMUData(const uint8_t *packet) {
	const uint32_t data_length = packet[4] |
	                             (packet[5] << 8) |
	                             (packet[6] << 16) |
	                             (packet[7] << 24);
	// Check length
	if (data_length != RAW_IMU_DATA_PACKET_LENGTH) {
		std::cerr << "Invalid raw IMU data length: " << data_length
				<< ", expected: " << RAW_IMU_DATA_PACKET_LENGTH << std::endl;
		return;
	}
	// Check CRC
	constexpr size_t crc_offset = ACENINNA_HEADER_LEN + RAW_IMU_DATA_PACKET_LENGTH;
	const uint16_t recv_crc = (packet[crc_offset] << 8) | packet[crc_offset + 1]; // MSB-first
	if (const uint16_t calc_crc = Tool::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + data_length);
		recv_crc != calc_crc) {
		std::cerr << "Raw IMU data CRC mismatch! Received: 0x" << std::hex << recv_crc
				<< " Calculated: 0x" << calc_crc << std::dec << std::endl;
		return;
	}
	// Parse IMU data
	const uint8_t *imu_data = &packet[ACENINNA_HEADER_LEN];
	RawIMUData imu{};
	imu.gps_week = imu_data[0] | (imu_data[1] << 8);
	imu.gps_millisecs = imu_data[2] | (imu_data[3] << 8) |
	                    (imu_data[4] << 16) | (imu_data[5] << 24);
	std::memcpy(&imu.acc_x, &imu_data[6], sizeof(float));
	std::memcpy(&imu.acc_y, &imu_data[10], sizeof(float));
	std::memcpy(&imu.acc_z, &imu_data[14], sizeof(float));
	std::memcpy(&imu.gyro_x, &imu_data[18], sizeof(float));
	std::memcpy(&imu.gyro_y, &imu_data[22], sizeof(float));
	std::memcpy(&imu.gyro_z, &imu_data[26], sizeof(float)); {
		std::lock_guard<std::mutex> lock(queue_mutex_);
		imu_queue_.push(imu);
	}
	cv_.notify_one();
}


void INSDeviceReceiver::ReceiveLoop() {
	int epfd = -1;
	if (!Tool::Ethernet::SetupEpollForFd(sock_fd_, epfd, EPOLLIN)) {
		return;
	}

    std::vector<uint8_t> buffer(buffer_size_);

	while (running_) {
	    constexpr int MAX_EVENTS = 4;
        epoll_event events[MAX_EVENTS];
        const int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: epoll_wait failed: " << strerror(errno) << std::endl;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == sock_fd_) {
                while (true) {
                    const ssize_t recv_len = recv(sock_fd_, buffer.data(), buffer.size(), 0);
                    if (recv_len < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::cerr << "Error: recv failed: " << strerror(errno) << std::endl;
                        running_ = false;
                        break;
                    }
                    if (recv_len == 0) {
                        std::cerr << "Warning: recv returned 0 bytes" << std::endl;
                        break;
                    }

                    if (recv_len < ETH_HEADER_LEN) {
                        continue;
                    }

                    const uint8_t *src_mac = &buffer[6];
                    if (std::memcmp(src_mac, target_mac_, 6) == 0) {
                        VerifyData(buffer.data(), recv_len);
                    }
                }
            }
        }
    }
    close(epfd);
}


void INSDeviceReceiver::WriterThread() {
	while (running_) {
		std::unique_lock<std::mutex> lock(queue_mutex_);

		// 等待新数据或退出信号
		cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
			return !imu_queue_.empty() || !gnss_queue_.empty() || !running_;
		});

		// 批量获取IMU数据
		std::vector<RawIMUData> imu_batch;
		while (!imu_queue_.empty() && imu_batch.size() < 100) {
			imu_batch.push_back(imu_queue_.front());
			imu_queue_.pop();
		}

		// 批量获取GNSS数据
		std::vector<GNSSSolutionData> gnss_batch;
		while (!gnss_queue_.empty() && gnss_batch.size() < 10) {
			// GNSS数据较少
			gnss_batch.push_back(gnss_queue_.front());
			gnss_queue_.pop();
		}

		// 解锁后写入文件
		lock.unlock();

		// 写入IMU数据
		if (save_to_file_ && imu_file_.is_open() && !imu_batch.empty()) {
			for (const auto &imu: imu_batch) {
				imu_file_ << imu.gps_week << ","
						<< imu.gps_millisecs << ","
						<< imu.acc_x << ","
						<< imu.acc_y << ","
						<< imu.acc_z << ","
						<< imu.gyro_x << ","
						<< imu.gyro_y << ","
						<< imu.gyro_z << "\n";
			}
			imu_file_.flush();
		}

		// 写入GNSS数据
		if (save_to_file_ && gnss_file_.is_open() && !gnss_batch.empty()) {
			for (const auto &gnss: gnss_batch) {
				gnss_file_ << gnss.gps_week << ","
						<< gnss.gps_millisecs << ","
						<< static_cast<int>(gnss.position_type) << ","
						<< std::fixed << std::setprecision(9)
						<< gnss.latitude << ","
						<< gnss.longitude << ","
						<< std::setprecision(3)
						<< gnss.height << ","
						<< gnss.latitude_std << ","
						<< gnss.longitude_std << ","
						<< gnss.height_std << ","
						<< static_cast<int>(gnss.num_of_SVs) << ","
						<< static_cast<int>(gnss.num_of_SVs_in_solution) << ","
						<< gnss.hdop << ","
						<< gnss.diffage << ","
						<< gnss.north_vel << ","
						<< gnss.east_vel << ","
						<< gnss.up_vel << ","
						<< gnss.north_vel_std << ","
						<< gnss.east_vel_std << ","
						<< gnss.up_vel_std << "\n";
			}
			gnss_file_.flush();
		}

		// 清理过大的队列
		lock.lock();
		while (imu_queue_.size() > 10000) {
			imu_queue_.pop();
		}
		while (gnss_queue_.size() > 1000) {
			// GNSS队列阈值更小
			gnss_queue_.pop();
		}
	}
}
