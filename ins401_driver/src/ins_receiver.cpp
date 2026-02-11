#include "ins_receiver.h"

#include <bitset>
#include <cstring>
#include <filesystem>
#include <net/if.h>
#include <spdlog/fmt/bundled/ranges.h>
#include <spdlog/fmt/std.h>
#include <sys/epoll.h>
#include <vector>

#include "ins401_protocol.h"
#include "ntrip_client.h"
#include "tool.h"


namespace {
    constexpr std::string_view kModule = "INS Receiver";
}


INSDeviceReceiver::INSDeviceReceiver(std::string iface, const std::string &device_mac, const bool save_to_file,
                                     std::string output_folder_path, double horizontal_std,
                                     bool enable_vrs) : interface_name_(std::move(iface)), save_to_file_(save_to_file),
                                                        output_folder_path_(std::move(output_folder_path)),
                                                        horizontal_std_(horizontal_std),
                                                        enable_vrs_(enable_vrs) {
    device_mac_ = Ethernet::FormatMACAddress(device_mac);
    socket_ptr_ = std::make_shared<EthernetSocket>(interface_name_, device_mac_, buffer_size_, true);
}


INSDeviceReceiver::~INSDeviceReceiver() {
    Stop();
    cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    if (imu_file_.is_open()) {
        imu_file_.close();
    }
}


void INSDeviceReceiver::Run() {
    if (save_to_file_) {
        running_.store(InitializeWritingFiles());
    }
    ReceiveLoop();
}


void INSDeviceReceiver::Stop() {
    running_.store(false);
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


void INSDeviceReceiver::SetNtripClient(NTRIPClient *client) {
    ntrip_client_.store(client, std::memory_order_release);
}


// std::string INSDeviceReceiver::WaitForFirstGga(const std::atomic<bool> *terminate_flag,
//                                                const std::chrono::milliseconds wait_step) {
//     std::unique_lock<std::mutex> lock(first_gga_mutex_);
//     while (!first_gga_ready_.load(std::memory_order_acquire)) {
//         if (terminate_flag && terminate_flag->load(std::memory_order_acquire)) {
//             break;
//         }
//         first_gga_cv_.wait_for(lock, wait_step);
//     }
//     return first_gga_from_device_;
// }


void INSDeviceReceiver::ReceiveLoop() {
    while (running_.load()) {
        auto response = socket_ptr_->Receive(100);
        if (response && !response->empty()) {
            VerifyDataFrame(response->data(), response->size());
        }
    }
}


void INSDeviceReceiver::VerifyDataFrame(const uint8_t *data, const size_t len) {
    // Basic length check
    if (len < 60) {
        return;
    }

    const uint8_t *packet = data + kEthernetHeaderSize;
    // Check Aceinna binary command start
    if (packet[0] == COMMAND_START_BYTES[0] && packet[1] == COMMAND_START_BYTES[1]) {
        const uint16_t recv_msg_id = packet[2] | (packet[3] << 8);
        const uint32_t data_length = packet[4] | (packet[5] << 8) | (packet[6] << 16) | (packet[7] << 24);
        switch (recv_msg_id) {
            case GNSS_SOLUTION_PACKET_MESSAGE_ID:
                if (data_length == GNSS_SOLUTION_PACKET_LENGTH) {
                    ProcessGNSSSolutionData(packet);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid GNSS solution data length: {}, expected: {}", data_length,
                                         GNSS_SOLUTION_PACKET_LENGTH));
                }
                break;
            case DIAGNOSTIC_MESSAGE_ID:
                if (data_length == DIAGNOSTIC_MESSAGE_LENGTH) {
                    ProcessDiagnosticMessage(packet);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid diagnostic message length: {}, expected: {}", data_length,
                                         DIAGNOSTIC_MESSAGE_LENGTH));
                }
                break;
            case RAW_IMU_DATA_MESSAGE_ID:
                if (data_length == RAW_IMU_DATA_LENGTH) {
                    ProcessRawIMUData(packet);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid raw IMU data length: {}, expected: {}", data_length,
                                         RAW_IMU_DATA_LENGTH));
                }
                break;
            case RTCM_ROVER_DATA_MESSAGE_ID:
                if (data_length >= 1 && data_length <= RTCM_ROVER_DATA_LENGTH_MAX) {
                    ProcessRTCMRoverData(packet, data_length);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid RTCM rover data length: {}, expected: 1-{}", data_length,
                                         RTCM_ROVER_DATA_LENGTH_MAX));
                }
                break;
            default:
                break;
        }
    } else if (packet[0] == NEMA_ASCII_START) {
        // Check NEMA ASCII start
        ProcessNMEAMessage(packet);
    } else {
        // Unknown packet type
        return;
    }
}


void INSDeviceReceiver::ProcessGNSSSolutionData(const uint8_t *packet) {
    // Check CRC
    constexpr size_t crc_offset = ACENINNA_HEADER_LEN + GNSS_SOLUTION_PACKET_LENGTH;
    const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8; // LSB-first
    const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + GNSS_SOLUTION_PACKET_LENGTH);
    if (recv_crc != calc_crc) {
        Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                             "GNSS solution data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}",
                             recv_crc, calc_crc));
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
        GNSSSolutionData previous_gnss = gnss_queue_.back();
        gnss_queue_.push(gnss);

        if (gnss.position_type != previous_gnss.position_type && gnss.position_type == 4) {
            Tool::LogMessage(spdlog::level::info, kModule, "Entered RTK_FIXED position type");
        } else if (gnss.position_type != previous_gnss.position_type && previous_gnss.position_type == 4) {
            Tool::LogMessage(spdlog::level::warn, kModule, "Lost RTK_FIXED position type");
        }
        // Consider std of latitude and longitude is sufficient
        double previous_std = (previous_gnss.latitude_std + previous_gnss.longitude_std) / 2;
        double current_std = (gnss.latitude_std + gnss.longitude_std) / 2;
        if (previous_std > horizontal_std_ && current_std <= horizontal_std_) {
            Tool::LogMessage(spdlog::level::info, kModule, fmt::format("Converged to {} m STD", horizontal_std_));
        } else if (previous_std <= horizontal_std_ && current_std > horizontal_std_) {
            Tool::LogMessage(spdlog::level::info, kModule,
                             fmt::format("Diverge outside {} m STD, stop scanning waiting for convergence",
                                         horizontal_std_));
        }
    }
    cv_.notify_one();
    // Fire GNSS callback outside the queue lock
    GnssCallback gnss_cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        gnss_cb = gnss_callback_;
    }
    if (gnss_cb) {
        gnss_cb(gnss);
    }
}


void INSDeviceReceiver::ProcessINSSolutionData(const uint8_t *packet) {
    // Check CRC
    constexpr size_t crc_offset = ACENINNA_HEADER_LEN + INS_SOLUTION_PACKET_LENGTH;
    const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8; // LSB-first
    const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + INS_SOLUTION_PACKET_LENGTH);
    if (recv_crc != calc_crc) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("INS solution data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}",
                                     recv_crc, calc_crc));
        return;
    }
    // Parse GNSS solution data
    const uint8_t *ins_data = &packet[ACENINNA_HEADER_LEN];
    INSSolutionData ins{};
    ins.gps_week = *reinterpret_cast<const uint16_t *>(ins_data);
    ins.gps_millisecs = *reinterpret_cast<const uint32_t *>(ins_data + 2);
    ins.ins_status = ins_data[6];
    ins.ins_position_type = ins_data[7];
    std::memcpy(&ins.latitude, ins_data + 8, sizeof(double));
    std::memcpy(&ins.longitude, ins_data + 16, sizeof(double));
    std::memcpy(&ins.height, ins_data + 24, sizeof(double));
    std::memcpy(&ins.north_vel, ins_data + 32, sizeof(float));
    std::memcpy(&ins.east_vel, ins_data + 36, sizeof(float));
    std::memcpy(&ins.up_vel, ins_data + 40, sizeof(float));
    std::memcpy(&ins.longitudinal_vel, ins_data + 44, sizeof(float));
    std::memcpy(&ins.lateral_vel, ins_data + 48, sizeof(float));
    std::memcpy(&ins.roll, ins_data + 52, sizeof(float));
    std::memcpy(&ins.pitch, ins_data + 56, sizeof(float));
    std::memcpy(&ins.heading, ins_data + 60, sizeof(float));
    std::memcpy(&ins.latitude_std, ins_data + 64, sizeof(float));
    std::memcpy(&ins.longitude_std, ins_data + 68, sizeof(float));
    std::memcpy(&ins.height_std, ins_data + 72, sizeof(float));
    std::memcpy(&ins.north_vel_std, ins_data + 76, sizeof(float));
    std::memcpy(&ins.east_vel_std, ins_data + 80, sizeof(float));
    std::memcpy(&ins.up_vel_std, ins_data + 84, sizeof(float));
    std::memcpy(&ins.long_vel_std, ins_data + 88, sizeof(float));
    std::memcpy(&ins.lat_vel_std, ins_data + 92, sizeof(float));
    std::memcpy(&ins.roll_std, ins_data + 96, sizeof(float));
    std::memcpy(&ins.pitch_std, ins_data + 100, sizeof(float));
    std::memcpy(&ins.heading_std, ins_data + 104, sizeof(float));
    ins.continent_id = *reinterpret_cast<const uint16_t *>(ins_data + 108);
    {
        std::lock_guard lock(queue_mutex_);
        if (ins_queue_.size() >= max_ins_queue_size_) {
            ins_queue_.pop();
        }
        ins_queue_.push(ins);
    }
    cv_.notify_one();
}


void INSDeviceReceiver::ProcessDiagnosticMessage(const uint8_t *packet) {
    // Check CRC
    constexpr size_t crc_offset = ACENINNA_HEADER_LEN + DIAGNOSTIC_MESSAGE_LENGTH;
    const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8; // LSB-first
    const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + DIAGNOSTIC_MESSAGE_LENGTH);
    if (recv_crc != calc_crc) {
        Tool::LogMessage(
            spdlog::level::warn, kModule, fmt::format(
                "Diagnostic message CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}\n", recv_crc,
                calc_crc));
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
    const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8; // LSB-first
    const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + RAW_IMU_DATA_LENGTH);
    if (recv_crc != calc_crc) {
        Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                             "Raw IMU data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}\n", recv_crc,
                             calc_crc));
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
    // Fire IMU callback outside the queue lock
    ImuCallback imu_cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        imu_cb = imu_callback_;
    }
    if (imu_cb) {
        imu_cb(imu);
    }
}


void INSDeviceReceiver::ProcessRTCMRoverData(const uint8_t *packet, size_t len) {
    // Check CRC
    size_t crc_offset = ACENINNA_HEADER_LEN + len;
    const uint16_t recv_crc = (packet[crc_offset]) | packet[crc_offset + 1] << 8; // LSB-first
    const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + len);
    if (recv_crc != calc_crc) {
        Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                             "RTCM rover data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}\n",
                             recv_crc, calc_crc));
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
                Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                     "NMEA message checksum mismatch! Received: 0x{:02x} Calculated: 0x{:02x}\n",
                                     recv_checksum, checksum));
                return;
            }
        } else {
            Tool::LogMessage(spdlog::level::warn, kModule, fmt::format("NMEA message missing checksum!"));
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
        if (enable_vrs_) {
            HandleGgaMessage(nmea_msg);
            NmeaCallback nmea_cb;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                nmea_cb = nmea_callback_;
            }
            if (nmea_cb) {
                nmea_cb(nmea_msg);
            }
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
    {
        std::lock_guard<std::mutex> lock(first_gga_mutex_);
        if (first_gga_from_device_.empty()) {
            first_gga_from_device_ = nmea;
            first_gga_ready_.store(true, std::memory_order_release);
        }
    }
    first_gga_cv_.notify_all();
    if (auto *client = ntrip_client_.load(std::memory_order_acquire)) {
        client->UpdateGgaFromNmea(nmea);
    }
}


void INSDeviceReceiver::WriterThread() {
    std::vector<GNSSSolutionData> gnss_batch;
    std::vector<INSSolutionData> ins_batch;
    std::vector<DiagnosticMessage> diagnostic_batch;
    std::vector<RawIMUData> imu_batch;
    std::vector<std::vector<uint8_t> > rtcm_rover_batch;
    std::vector<std::string> nmea_batch;
    gnss_batch.reserve(gnss_write_batch_size_);
    ins_batch.reserve(ins_write_batch_size_);
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
            ins_batch.clear();
            const size_t ins_to_take = std::min(ins_write_batch_size_, ins_queue_.size());
            for (size_t i = 0; i < ins_to_take; ++i) {
                ins_batch.emplace_back(ins_queue_.front());
                ins_queue_.pop();
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
            if (ins_queue_.size() > max_ins_queue_size_) {
                const size_t to_remove = ins_queue_.size() - max_ins_queue_size_ / 2;
                for (size_t i = 0; i < to_remove; ++i) {
                    ins_queue_.pop();
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
                if (ins_file_.is_open()) {
                    ins_file_.flush();
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
    buffer.reserve(batch.size() * 256); // Reserve approximate size
    for (const auto &gnss: batch) {
        fmt::format_to(std::back_inserter(buffer),
                       "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
                       gnss.gps_week,
                       gnss.gps_millisecs,
                       gnss.position_type,
                       gnss.latitude,
                       gnss.longitude,
                       gnss.height,
                       gnss.latitude_std,
                       gnss.longitude_std,
                       gnss.height_std,
                       gnss.num_of_SVs,
                       gnss.num_of_SVs_in_solution,
                       gnss.hdop,
                       gnss.diffage,
                       gnss.north_vel,
                       gnss.east_vel,
                       gnss.up_vel,
                       gnss.north_vel_std,
                       gnss.east_vel_std,
                       gnss.up_vel_std
        );
    }
    gnss_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}


void INSDeviceReceiver::WriteINSBatch(const std::vector<INSSolutionData> &batch) {
    if (!ins_file_.is_open() || batch.empty()) {
        return;
    }
    fmt::memory_buffer buffer;
    buffer.reserve(batch.size() * 512);
    for (const auto &ins: batch) {
        fmt::format_to(std::back_inserter(buffer),
                       "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
                       ins.gps_week,
                       ins.gps_millisecs,
                       ins.ins_status,
                       ins.ins_position_type,
                       ins.latitude,
                       ins.longitude,
                       ins.height,
                       ins.north_vel,
                       ins.east_vel,
                       ins.up_vel,
                       ins.longitudinal_vel,
                       ins.lateral_vel,
                       ins.roll,
                       ins.pitch,
                       ins.heading,
                       ins.latitude_std,
                       ins.longitude_std,
                       ins.height_std,
                       ins.north_vel_std,
                       ins.east_vel_std,
                       ins.up_vel_std,
                       ins.long_vel_std,
                       ins.lat_vel_std,
                       ins.roll_std,
                       ins.pitch_std,
                       ins.heading_std,
                       ins.continent_id
        );
    }
    ins_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}


void INSDeviceReceiver::WriteDiagnosticBatch(const std::vector<DiagnosticMessage> &batch) {
    if (!diagnostic_file_.is_open() || batch.empty()) {
        return;
    }
    fmt::memory_buffer buffer;
    buffer.reserve(batch.size() * 128); // Reserve approximate size
    for (const auto &diag: batch) {
        fmt::format_to(std::back_inserter(buffer),
                       "{},{},{},{},{},{}\n",
                       diag.gps_week,
                       diag.gps_millisecs,
                       fmt::format("[{}]", fmt::join(diag.device_status, ",")),
                       diag.imu_temperature,
                       diag.mcu_temperature,
                       diag.gnss_chip_temperature
        );
    }
    diagnostic_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}


void INSDeviceReceiver::WriteIMUBatch(const std::vector<RawIMUData> &batch) {
    if (!imu_file_.is_open() || batch.empty()) {
        return;
    }
    fmt::memory_buffer buffer;
    buffer.reserve(batch.size() * 128); // Reserve approximate size
    for (const auto &imu: batch) {
        fmt::format_to(std::back_inserter(buffer),
                       "{},{},{},{},{},{},{},{}\n",
                       imu.gps_week,
                       imu.gps_millisecs,
                       imu.acc_x,
                       imu.acc_y,
                       imu.acc_z,
                       imu.gyro_x,
                       imu.gyro_y,
                       imu.gyro_z
        );
    }
    imu_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}


void INSDeviceReceiver::WriteRTCMRoverBatch(const std::vector<std::vector<uint8_t> > &batch) {
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
    buffer.reserve(batch.size() * 128); // Reserve approximate size
    for (const auto &nmea: batch) {
        fmt::format_to(std::back_inserter(buffer), "{}", nmea);
    }
    nmea_file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}


bool INSDeviceReceiver::InitializeWritingFiles() {
    if (save_to_file_) {
        if (output_folder_path_.empty()) {
            Tool::LogMessage(spdlog::level::err, kModule, "Error: Output folder path is empty!");
            return false;
        }

        std::string_view timestamp = Tool::Utility::SplitString(output_folder_path_, '/').back();
        std::string gnss_filename = fmt::format("{}/gnss_data_{}.txt", output_folder_path_, timestamp);
        std::string ins_filename = fmt::format("{}/ins_data_{}.txt", output_folder_path_, timestamp);
        std::string diagnostic_filename = fmt::format("{}/diagnostic_data_{}.txt", output_folder_path_, timestamp);
        std::string imu_filename = fmt::format("{}/imu_data_{}.txt", output_folder_path_, timestamp);
        std::string rtcm_rover_filename = fmt::format("{}/rtcm_rover_data_{}.rtcm3", output_folder_path_, timestamp);
        std::string nmea_filename = fmt::format("{}/nmea_message_{}.txt", output_folder_path_, timestamp);

        gnss_file_buffer_.resize(write_buffer_size_);
        gnss_file_.open(gnss_filename, std::ios::out);
        if (gnss_file_.is_open()) {
            gnss_file_.rdbuf()->pubsetbuf(gnss_file_buffer_.data(),
                                          static_cast<std::streamsize>(gnss_file_buffer_.size()));
            fmt::print(gnss_file_,
                       "GPS_Week,GPS_MS[ms],Position_Type,Latitude[deg],Longitude[deg],Height[m],Latitude_STD[m],Longitude_STD[m],Height_STD[m],Num_of_SVs,Num_of_SVs_in_Solution,Hdop,Diffage[s],North_Vel[m/s],East_Vel[m/s],Up_Vel[m/s],North_Vel_STD[m/s],East_Vel_STD[m/s],Up_Vel_STD[m/s]\n");
        }

        ins_file_buffer_.resize(write_buffer_size_);
        ins_file_.open(ins_filename, std::ios::out);
        if (ins_file_.is_open()) {
            ins_file_.rdbuf()->pubsetbuf(ins_file_buffer_.data(),
                                         static_cast<std::streamsize>(ins_file_buffer_.size()));
            fmt::print(ins_file_,
                       "GPS_Week,GPS_MS[ms],INS_Status,INS_Position_Type,Latitude[deg],Longitude[deg],Height[m],North_Vel[m/s],East_Vel[m/s],Up_Vel[m/s],Longitudinal_Vel[m/s],Lateral_Vel[m/s],Roll[deg],Pitch[deg],Heading[deg],Latitude_STD[m],Longitude_STD[m],Height_STD[m],North_Vel_STD[m/s],East_Vel_STD[m/s],Up_Vel_STD[m/s],Longitudinal_Vel_STD[m/s],Lateral_Vel_STD[m/s],Roll_STD[deg],Pitch[STD],Heading_STD[deg],Continent_ID\n");
        }

        diagnostic_file_buffer_.resize(write_buffer_size_);
        diagnostic_file_.open(diagnostic_filename, std::ios::out);
        if (diagnostic_file_.is_open()) {
            diagnostic_file_.rdbuf()->pubsetbuf(diagnostic_file_buffer_.data(),
                                                static_cast<std::streamsize>(diagnostic_file_buffer_.size()));
            fmt::print(diagnostic_file_,
                       "GPS_Week,GPS_MS[ms],Device_Status,IMU_Temperature[°C],MCU_Temperature[°C],GNSS_Chip_Temperature[°C]\n");
        }

        imu_file_buffer_.resize(write_buffer_size_);
        imu_file_.open(imu_filename, std::ios::out);
        if (imu_file_.is_open()) {
            imu_file_.rdbuf()->pubsetbuf(imu_file_buffer_.data(),
                                         static_cast<std::streamsize>(imu_file_buffer_.size()));
            fmt::print(imu_file_,
                       "GPS_Week,GPS_MS[ms],Acc_X[m/s^2],Acc_Y[m/s^2],Acc_Z[m/s^2],Gyro_X[deg/s],Gyro_Y[deg/s],Gyro_Z[deg/s]\n");
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
            nmea_file_.rdbuf()->pubsetbuf(nmea_file_buffer_.data(),
                                          static_cast<std::streamsize>(nmea_file_buffer_.size()));
        }

        writer_thread_ = std::thread(&INSDeviceReceiver::WriterThread, this);
    }
    return true;
}
