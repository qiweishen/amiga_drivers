#include "ins_driver.h"



bool EthernetINSReceiver::Initialize() {
    sock_fd_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock_fd_ < 0) {
        std::cerr << "Failed to create raw socket. Run with sudo." << std::endl;
        return false;
    }

    ifreq ifr{};
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface_name_.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock_fd_, SIOCGIFINDEX, &ifr) < 0) {
        std::cerr << "Failed to get interface index for " << interface_name_ << std::endl;
        close(sock_fd_);
        return false;
    }

    sockaddr_ll sll{};
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(sock_fd_, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0) {
        std::cerr << "Failed to bind to interface" << std::endl;
        close(sock_fd_);
        return false;
    }

    packet_mreq mr{};
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = ifr.ifr_ifindex;
    mr.mr_type = PACKET_MR_PROMISC;

    if (setsockopt(sock_fd_, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0) {
        std::cerr << "Warning: Failed to set promiscuous mode" << std::endl;
    }

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::cout << "Ethernet IMU Receiver initialized on interface: " << interface_name_ << std::endl;
    std::cout << "Listening for packets from MAC: ";
    std::cout << std::endl;

    return true;
}


void EthernetINSReceiver::VerifyData(const uint8_t *data, const size_t len) {
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


void EthernetINSReceiver::ProcessGNSSSolutionData(const uint8_t *packet) {
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
    if (const uint16_t calc_crc = Tool::INS401::CalcCRC(&packet[2], 2 + 4 + data_length);
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
    std::memcpy(&gnss.up_vel_std, &gnss_solution_data[73], sizeof(float));
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        gnss_queue_.push(gnss);
    }
    cv_.notify_one();
}


void EthernetINSReceiver::ProcessRawIMUData(const uint8_t *packet) {
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
    if (const uint16_t calc_crc = Tool::INS401::CalcCRC(&packet[2], 2 + 4 + data_length);
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
    std::memcpy(&imu.gyro_z, &imu_data[26], sizeof(float));
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        imu_queue_.push(imu);
    }
    cv_.notify_one();
}


void EthernetINSReceiver::ReceiveLoop() {
    while (running_) {
        const ssize_t recv_len = recv(sock_fd_, buffer_.data(), buffer_.size(), 0);

        if (recv_len < 0) {
            continue;
        }

        if (recv_len < ETH_HEADER_LEN) {
            continue;
        }

        if (const uint8_t *src_mac = &buffer_[6]; memcmp(src_mac, target_mac_, 6) == 0) {
            VerifyData(buffer_.data(), recv_len);
        }
    }
}


bool EthernetINSReceiver::GetGNSSData(std::vector<GNSSSolutionData>& data, size_t max_count) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
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


bool EthernetINSReceiver::GetIMUData(std::vector<RawIMUData>& data, size_t max_count) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
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


void EthernetINSReceiver::WriterThread() {
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
        while (!gnss_queue_.empty() && gnss_batch.size() < 10) {  // GNSS数据较少
            gnss_batch.push_back(gnss_queue_.front());
            gnss_queue_.pop();
        }

        // 解锁后写入文件
        lock.unlock();

        // 写入IMU数据
        if (save_to_file_ && imu_file_.is_open() && !imu_batch.empty()) {
            for (const auto& imu : imu_batch) {
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
            for (const auto& gnss : gnss_batch) {
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
        while (gnss_queue_.size() > 1000) {  // GNSS队列阈值更小
            gnss_queue_.pop();
        }
    }
}