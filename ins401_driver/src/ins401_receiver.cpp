#include "ins401_receiver.h"

#include <bitset>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <net/if.h>
#include <spdlog/fmt/bundled/ranges.h>
#include <spdlog/fmt/std.h>
#include <sys/epoll.h>
#include <utility>
#include <vector>

#include "initialization_monitor.h"
#include "ins401_protocol.h"
#include "ntrip_client.h"
#include "tool.h"


namespace {
    constexpr std::string_view kModule = "INS Receiver";
    constexpr std::size_t kPostProcessBatchSize = 1024;
}


INSDeviceReceiver::INSDeviceReceiver(std::string iface, std::string device_mac, const Config &config) : interface_name_(
    std::move(iface)) {
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
    running_.store(InitializeWritingFiles());
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
}


// Parse an Ethernet frame containing an Aceinna binary packet:
// [0x5555(2) | MsgID(2) | PayloadLen(4) | Payload(N) | CRC16(2)]
// or an NMEA ASCII sentence starting with '$'.
void INSDeviceReceiver::VerifyDataFrame(const uint8_t *data, const size_t len) {
    if (len < 60) {
        return;
    }

    stat_total_bytes_received_.fetch_add(len, std::memory_order_relaxed);

    // Check Aceinna binary command start
    const uint8_t *packet = data + kEthernetHeaderSize;
    if (packet[0] == COMMAND_START_BYTES[0] && packet[1] == COMMAND_START_BYTES[1]) {
        const uint16_t recv_msg_id = packet[2] | (packet[3] << 8);
        const uint32_t data_length = packet[4] | (packet[5] << 8) | (packet[6] << 16) | (packet[7] << 24);
        switch (recv_msg_id) {
            case GNSS_SOLUTION_PACKET_MESSAGE_ID:
                if (data_length == GNSS_SOLUTION_PACKET_LENGTH) {
                    HandleGNSSSolutionPacket(packet);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid GNSS solution data length: {}, expected: {}", data_length,
                                         GNSS_SOLUTION_PACKET_LENGTH));
                }
                break;
            case INS_SOLUTION_PACKET_MESSAGE_ID:
                if (data_length == INS_SOLUTION_PACKET_LENGTH) {
                    HandleINSSolutionPacket(packet);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid INS solution data length: {}, expected: {}", data_length,
                                         INS_SOLUTION_PACKET_LENGTH));
                }
                break;
            case DIAGNOSTIC_MESSAGE_ID:
                if (data_length == DIAGNOSTIC_MESSAGE_LENGTH) {
                    HandleDiagnosticPacket(packet);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid diagnostic message length: {}, expected: {}", data_length,
                                         DIAGNOSTIC_MESSAGE_LENGTH));
                }
                break;
            case RAW_IMU_DATA_MESSAGE_ID:
                if (data_length == RAW_IMU_DATA_LENGTH) {
                    HandleRawIMUPacket(packet);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid raw IMU data length: {}, expected: {}", data_length,
                                         RAW_IMU_DATA_LENGTH));
                }
                break;
            case RTCM_ROVER_DATA_MESSAGE_ID:
                if (data_length >= 1 && data_length <= RTCM_ROVER_DATA_LENGTH_MAX) {
                    HandleRTCMRoverPacket(packet, data_length);
                } else {
                    Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                         "Invalid RTCM rover data length: {}, expected: 1-{}", data_length,
                                         RTCM_ROVER_DATA_LENGTH_MAX));
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
    const uint16_t recv_crc = packet[crc_offset] | (packet[crc_offset + 1] << 8); // LSB-first
    const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + GNSS_SOLUTION_PACKET_LENGTH);
    if (recv_crc != calc_crc) {
        stat_gnss_crc_errors_.fetch_add(1, std::memory_order_relaxed);
        Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                             "GNSS solution data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}",
                             recv_crc, calc_crc));
        return;
    }

    stat_gnss_packets_.fetch_add(1, std::memory_order_relaxed);

    const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
    if (gnss_bin_file_.is_open()) {
        gnss_bin_file_.write(reinterpret_cast<const char *>(payload),
                             static_cast<std::streamsize>(GNSS_SOLUTION_PACKET_LENGTH));
    }

    // Only parse GNSS packet in real-time under the need of GNSS Checking.
    if (check_gnss_) {
        GNSSSolutionData gnss = ParseGNSSSolutionData(payload);
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
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("INS solution data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}",
                                     recv_crc, calc_crc));
        return;
    }

    stat_ins_packets_.fetch_add(1, std::memory_order_relaxed);

    const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
    if (ins_bin_file_.is_open()) {
        ins_bin_file_.write(reinterpret_cast<const char *>(payload),
                            static_cast<std::streamsize>(INS_SOLUTION_PACKET_LENGTH));
    }
}


void INSDeviceReceiver::HandleDiagnosticPacket(const uint8_t *packet) {
    constexpr size_t crc_offset = ACEINNA_HEADER_LEN + DIAGNOSTIC_MESSAGE_LENGTH;
    const uint16_t recv_crc = packet[crc_offset] | (packet[crc_offset + 1] << 8);
    const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + DIAGNOSTIC_MESSAGE_LENGTH);
    if (recv_crc != calc_crc) {
        stat_diagnostic_crc_errors_.fetch_add(1, std::memory_order_relaxed);
        Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                             "Diagnostic message CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}",
                             recv_crc, calc_crc));
        return;
    }

    stat_diagnostic_packets_.fetch_add(1, std::memory_order_relaxed);

    const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
    if (diagnostic_bin_file_.is_open()) {
        diagnostic_bin_file_.write(reinterpret_cast<const char *>(payload),
                                   static_cast<std::streamsize>(DIAGNOSTIC_MESSAGE_LENGTH));
    }
}


void INSDeviceReceiver::HandleRawIMUPacket(const uint8_t *packet) {
    constexpr size_t crc_offset = ACEINNA_HEADER_LEN + RAW_IMU_DATA_LENGTH;
    const uint16_t recv_crc = packet[crc_offset] | (packet[crc_offset + 1] << 8);
    const uint16_t calc_crc = Ethernet::CRC::CalculateINS401_CRC16(&packet[2], 2 + 4 + RAW_IMU_DATA_LENGTH);
    if (recv_crc != calc_crc) {
        stat_imu_crc_errors_.fetch_add(1, std::memory_order_relaxed);
        Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                             "Raw IMU data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}",
                             recv_crc, calc_crc));
        return;
    }

    stat_imu_packets_.fetch_add(1, std::memory_order_relaxed);

    const uint8_t *payload = &packet[ACEINNA_HEADER_LEN];
    if (imu_bin_file_.is_open()) {
        imu_bin_file_.write(reinterpret_cast<const char *>(payload),
                            static_cast<std::streamsize>(RAW_IMU_DATA_LENGTH));
    }

    RawIMUData imu = ParseRawIMUData(payload);
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
        Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                             "RTCM rover data CRC mismatch! Received: 0x{:04x} Calculated: 0x{:04x}",
                             recv_crc, calc_crc));
        return;
    }

    stat_rtcm_rover_packets_.fetch_add(1, std::memory_order_relaxed);

    const uint8_t *rtcm_data = &packet[ACEINNA_HEADER_LEN];
    if (rtcm_rover_file_.is_open()) {
        rtcm_rover_file_.write(reinterpret_cast<const char *>(rtcm_data), static_cast<std::streamsize>(len));
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
                Tool::LogMessage(spdlog::level::warn, kModule, fmt::format(
                                     "NMEA message checksum mismatch! Received: 0x{:02x} Calculated: 0x{:02x}",
                                     recv_checksum, checksum));
                return;
            }
        } else {
            stat_nmea_checksum_errors_.fetch_add(1, std::memory_order_relaxed);
            Tool::LogMessage(spdlog::level::warn, kModule, "NMEA message missing checksum!");
            return;
        }

        stat_nmea_messages_.fetch_add(1, std::memory_order_relaxed);

        if (nmea_file_.is_open()) {
            nmea_file_.write(nmea_msg.data(), static_cast<std::streamsize>(nmea_msg.size()));
        }

        if (use_vrs_ || !first_gga_blh_ready_.load(std::memory_order_acquire)) {
            HandleGgaMessage(nmea_msg);
        }
    }
}


GNSSSolutionData INSDeviceReceiver::ParseGNSSSolutionData(const uint8_t *payload) {
    GNSSSolutionData gnss{};
    std::memcpy(&gnss.gps_week, payload, sizeof(uint16_t));
    std::memcpy(&gnss.gps_millisecs, payload + 2, sizeof(uint32_t));
    gnss.position_type = payload[6];
    std::memcpy(&gnss.latitude, payload + 7, sizeof(double));
    std::memcpy(&gnss.longitude, payload + 15, sizeof(double));
    std::memcpy(&gnss.height, payload + 23, sizeof(double));
    std::memcpy(&gnss.latitude_std, payload + 31, sizeof(float));
    std::memcpy(&gnss.longitude_std, payload + 35, sizeof(float));
    std::memcpy(&gnss.height_std, payload + 39, sizeof(float));
    gnss.num_of_SVs = payload[43];
    gnss.num_of_SVs_in_solution = payload[44];
    std::memcpy(&gnss.hdop, payload + 45, sizeof(float));
    std::memcpy(&gnss.diffage, payload + 49, sizeof(float));
    std::memcpy(&gnss.north_vel, payload + 53, sizeof(float));
    std::memcpy(&gnss.east_vel, payload + 57, sizeof(float));
    std::memcpy(&gnss.up_vel, payload + 61, sizeof(float));
    std::memcpy(&gnss.north_vel_std, payload + 65, sizeof(float));
    std::memcpy(&gnss.east_vel_std, payload + 69, sizeof(float));
    std::memcpy(&gnss.up_vel_std, payload + 73, sizeof(float));
    return gnss;
}


INSSolutionData INSDeviceReceiver::ParseINSSolutionData(const uint8_t *payload) {
    INSSolutionData ins{};
    std::memcpy(&ins.gps_week, payload, sizeof(uint16_t));
    std::memcpy(&ins.gps_millisecs, payload + 2, sizeof(uint32_t));
    ins.ins_status = payload[6];
    ins.ins_position_type = payload[7];
    std::memcpy(&ins.latitude, payload + 8, sizeof(double));
    std::memcpy(&ins.longitude, payload + 16, sizeof(double));
    std::memcpy(&ins.height, payload + 24, sizeof(double));
    std::memcpy(&ins.north_vel, payload + 32, sizeof(float));
    std::memcpy(&ins.east_vel, payload + 36, sizeof(float));
    std::memcpy(&ins.up_vel, payload + 40, sizeof(float));
    std::memcpy(&ins.longitudinal_vel, payload + 44, sizeof(float));
    std::memcpy(&ins.lateral_vel, payload + 48, sizeof(float));
    std::memcpy(&ins.roll, payload + 52, sizeof(float));
    std::memcpy(&ins.pitch, payload + 56, sizeof(float));
    std::memcpy(&ins.heading, payload + 60, sizeof(float));
    std::memcpy(&ins.latitude_std, payload + 64, sizeof(float));
    std::memcpy(&ins.longitude_std, payload + 68, sizeof(float));
    std::memcpy(&ins.height_std, payload + 72, sizeof(float));
    std::memcpy(&ins.north_vel_std, payload + 76, sizeof(float));
    std::memcpy(&ins.east_vel_std, payload + 80, sizeof(float));
    std::memcpy(&ins.up_vel_std, payload + 84, sizeof(float));
    std::memcpy(&ins.long_vel_std, payload + 88, sizeof(float));
    std::memcpy(&ins.lat_vel_std, payload + 92, sizeof(float));
    std::memcpy(&ins.roll_std, payload + 96, sizeof(float));
    std::memcpy(&ins.pitch_std, payload + 100, sizeof(float));
    std::memcpy(&ins.heading_std, payload + 104, sizeof(float));
    std::memcpy(&ins.continent_id, payload + 108, sizeof(uint16_t));
    return ins;
}


DiagnosticMessage INSDeviceReceiver::ParseDiagnosticMessage(const uint8_t *payload) {
    DiagnosticMessage diag{};
    std::memcpy(&diag.gps_week, payload, sizeof(uint16_t));
    std::memcpy(&diag.gps_millisecs, payload + 2, sizeof(uint32_t));
    uint32_t status_value;
    std::memcpy(&status_value, payload + 6, sizeof(uint32_t));
    const std::bitset<32> bs(status_value);
    for (int i = 0; i < 32; ++i) {
        diag.device_status[i] = bs[i];
    }
    std::memcpy(&diag.imu_temperature, payload + 10, sizeof(float));
    std::memcpy(&diag.mcu_temperature, payload + 14, sizeof(float));
    std::memcpy(&diag.gnss_chip_temperature, payload + 18, sizeof(float));
    return diag;
}


RawIMUData INSDeviceReceiver::ParseRawIMUData(const uint8_t *payload) {
    RawIMUData imu{};
    std::memcpy(&imu.gps_week, payload, sizeof(uint16_t));
    std::memcpy(&imu.gps_millisecs, payload + 2, sizeof(uint32_t));
    std::memcpy(&imu.acc_x, payload + 6, sizeof(float));
    std::memcpy(&imu.acc_y, payload + 10, sizeof(float));
    std::memcpy(&imu.acc_z, payload + 14, sizeof(float));
    std::memcpy(&imu.gyro_x, payload + 18, sizeof(float));
    std::memcpy(&imu.gyro_y, payload + 22, sizeof(float));
    std::memcpy(&imu.gyro_z, payload + 26, sizeof(float));
    return imu;
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
        if (fields[3] == "S") latitude = -latitude;

        // Longitude: dddmm.mmmm -> decimal degrees -> radians
        double lon_raw = std::stod(fields[4]);
        double lon_deg = std::floor(lon_raw / 100.0);
        double longitude = lon_deg + (lon_raw - lon_deg * 100.0) / 60.0;
        if (fields[5] == "W") longitude = -longitude;

        // MSL altitude + geoid separation = ellipsoidal height
        double altitude = fields[9].empty() ? 0.0 : std::stod(fields[9]);
        double geoid_sep = (fields.size() > 11 && !fields[11].empty()) ? std::stod(fields[11]) : 0.0;

        return Eigen::Vector3d(latitude * M_PI / 180.0, longitude * M_PI / 180.0, altitude + geoid_sep);
    } catch (...) {
        Tool::LogMessage(spdlog::level::warn, kModule, "Failed to parse numeric fields in GGA sentence");
        return std::nullopt;
    }
}


void INSDeviceReceiver::CloseAllFiles() {
    if (gnss_bin_file_.is_open()) {
        gnss_bin_file_.close();
    }
    if (ins_bin_file_.is_open()) {
        ins_bin_file_.close();
    }
    if (imu_bin_file_.is_open()) {
        imu_bin_file_.close();
    }
    if (diagnostic_bin_file_.is_open()) {
        diagnostic_bin_file_.close();
    }
    if (rtcm_rover_file_.is_open()) {
        rtcm_rover_file_.close();
    }
    if (nmea_file_.is_open()) {
        nmea_file_.close();
    }
}


bool INSDeviceReceiver::InitializeWritingFiles() {
    if (output_folder_path_.empty()) {
        Tool::LogMessage(spdlog::level::err, kModule, "Error: Output folder path is empty!");
        return false;
    }

    std::string binary_folder_path = fmt::format("{}/{}", output_folder_path_, "bin");
    std::filesystem::create_directories(binary_folder_path);

    gnss_bin_path_ = fmt::format("{}/gnss_{}.bin", binary_folder_path, timestamp_);
    gnss_csv_path_ = fmt::format("{}/gnss_{}.csv", output_folder_path_, timestamp_);

    ins_bin_path_ = fmt::format("{}/ins_{}.bin", binary_folder_path, timestamp_);
    ins_csv_path_ = fmt::format("{}/ins_{}.csv", output_folder_path_, timestamp_);

    imu_bin_path_ = fmt::format("{}/imu_{}.bin", binary_folder_path, timestamp_);
    imu_csv_path_ = fmt::format("{}/imu_{}.csv", output_folder_path_, timestamp_);

    diagnostic_bin_path_ = fmt::format("{}/diagnostic_{}.bin", binary_folder_path, timestamp_);
    diagnostic_csv_path_ = fmt::format("{}/diagnostic_{}.csv", output_folder_path_, timestamp_);

    std::string rtcm_rover_filename = fmt::format("{}/rtcm_rover_{}.rtcm3", output_folder_path_, timestamp_);
    std::string nmea_filename = fmt::format("{}/nmea_{}.nmea", output_folder_path_, timestamp_);

    gnss_bin_buffer_.resize(write_buffer_size_);
    gnss_bin_file_.rdbuf()->pubsetbuf(gnss_bin_buffer_.data(),
                                      static_cast<std::streamsize>(gnss_bin_buffer_.size()));
    gnss_bin_file_.open(gnss_bin_path_, std::ios::out | std::ios::binary);

    ins_bin_buffer_.resize(write_buffer_size_);
    ins_bin_file_.rdbuf()->pubsetbuf(ins_bin_buffer_.data(),
                                     static_cast<std::streamsize>(ins_bin_buffer_.size()));
    ins_bin_file_.open(ins_bin_path_, std::ios::out | std::ios::binary);

    imu_bin_buffer_.resize(write_buffer_size_);
    imu_bin_file_.rdbuf()->pubsetbuf(imu_bin_buffer_.data(),
                                     static_cast<std::streamsize>(imu_bin_buffer_.size()));
    imu_bin_file_.open(imu_bin_path_, std::ios::out | std::ios::binary);

    diagnostic_bin_buffer_.resize(write_buffer_size_);
    diagnostic_bin_file_.rdbuf()->pubsetbuf(diagnostic_bin_buffer_.data(),
                                            static_cast<std::streamsize>(diagnostic_bin_buffer_.size()));
    diagnostic_bin_file_.open(diagnostic_bin_path_, std::ios::out | std::ios::binary);

    rtcm_rover_file_buffer_.resize(write_buffer_size_);
    rtcm_rover_file_.rdbuf()->pubsetbuf(rtcm_rover_file_buffer_.data(),
                                        static_cast<std::streamsize>(rtcm_rover_file_buffer_.size()));
    rtcm_rover_file_.open(rtcm_rover_filename, std::ios::out | std::ios::binary);

    nmea_file_buffer_.resize(write_buffer_size_);
    nmea_file_.rdbuf()->pubsetbuf(nmea_file_buffer_.data(),
                                  static_cast<std::streamsize>(nmea_file_buffer_.size()));
    nmea_file_.open(nmea_filename, std::ios::out);

    return true;
}


void INSDeviceReceiver::ProcessBinaryFiles() {
    CloseAllFiles();

    Tool::LogMessage(spdlog::level::info, kModule, "Parsing binary data files...");
    ProcessGNSSBinaryFile();
    ProcessINSBinaryFile();
    ProcessIMUBinaryFile();
    ProcessDiagnosticBinaryFile();
}


// Read fixed-size binary records written during collection and convert to CSV.
void INSDeviceReceiver::ProcessGNSSBinaryFile() {
    std::ifstream bin_in(gnss_bin_path_, std::ios::in | std::ios::binary);
    if (!bin_in.is_open()) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("Cannot open GNSS binary file: {}", gnss_bin_path_));
        return;
    }

    std::ofstream csv_out(gnss_csv_path_, std::ios::out);
    if (!csv_out.is_open()) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("Cannot create GNSS CSV file: {}", gnss_csv_path_));
        return;
    }

    std::vector<char> csv_buffer(write_buffer_size_);
    csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
    fmt::print(csv_out,
               "GPS_Week,GPS_MS[ms],Position_Type,Latitude[deg],Longitude[deg],Height[m],"
               "Latitude_STD[m],Longitude_STD[m],Height_STD[m],Num_of_SVs,Num_of_SVs_in_Solution,"
               "Hdop,Diffage[s],North_Vel[m/s],East_Vel[m/s],Up_Vel[m/s],"
               "North_Vel_STD[m/s],East_Vel_STD[m/s],Up_Vel_STD[m/s]\n");

    std::vector<uint8_t> record(GNSS_SOLUTION_PACKET_LENGTH);
    fmt::memory_buffer fmt_buf;
    std::size_t count = 0;

    while (bin_in.read(reinterpret_cast<char *>(record.data()),
                       static_cast<std::streamsize>(GNSS_SOLUTION_PACKET_LENGTH))) {
        const auto gnss = ParseGNSSSolutionData(record.data());
        fmt::format_to(std::back_inserter(fmt_buf),
                       "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
                       gnss.gps_week, gnss.gps_millisecs, gnss.position_type,
                       gnss.latitude, gnss.longitude, gnss.height,
                       gnss.latitude_std, gnss.longitude_std, gnss.height_std,
                       gnss.num_of_SVs, gnss.num_of_SVs_in_solution,
                       gnss.hdop, gnss.diffage,
                       gnss.north_vel, gnss.east_vel, gnss.up_vel,
                       gnss.north_vel_std, gnss.east_vel_std, gnss.up_vel_std);
        if (++count % kPostProcessBatchSize == 0) {
            csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
            fmt_buf.clear();
        }
    }

    if (fmt_buf.size() > 0) {
        csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
    }

    Tool::LogMessage(spdlog::level::info, kModule, fmt::format("    - GNSS: {} records processed", count));
}


// Read fixed-size binary records written during collection and convert to CSV.
void INSDeviceReceiver::ProcessINSBinaryFile() {
    std::ifstream bin_in(ins_bin_path_, std::ios::in | std::ios::binary);
    if (!bin_in.is_open()) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("Cannot open INS binary file: {}", ins_bin_path_));
        return;
    }

    std::ofstream csv_out(ins_csv_path_, std::ios::out);
    if (!csv_out.is_open()) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("Cannot create INS CSV file: {}", ins_csv_path_));
        return;
    }

    std::vector<char> csv_buffer(write_buffer_size_);
    csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
    fmt::print(csv_out,
               "GPS_Week,GPS_MS[ms],INS_Status,INS_Position_Type,Latitude[deg],Longitude[deg],Height[m],"
               "North_Vel[m/s],East_Vel[m/s],Up_Vel[m/s],Longitudinal_Vel[m/s],Lateral_Vel[m/s],"
               "Roll[deg],Pitch[deg],Heading[deg],"
               "Latitude_STD[m],Longitude_STD[m],Height_STD[m],"
               "North_Vel_STD[m/s],East_Vel_STD[m/s],Up_Vel_STD[m/s],"
               "Longitudinal_Vel_STD[m/s],Lateral_Vel_STD[m/s],"
               "Roll_STD[deg],Pitch_STD[deg],Heading_STD[deg],Continent_ID\n");

    std::vector<uint8_t> record(INS_SOLUTION_PACKET_LENGTH);
    fmt::memory_buffer fmt_buf;
    std::size_t count = 0;

    while (bin_in.read(reinterpret_cast<char *>(record.data()),
                       static_cast<std::streamsize>(INS_SOLUTION_PACKET_LENGTH))) {
        const auto ins = ParseINSSolutionData(record.data());
        fmt::format_to(std::back_inserter(fmt_buf),
                       "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
                       ins.gps_week, ins.gps_millisecs, ins.ins_status, ins.ins_position_type,
                       ins.latitude, ins.longitude, ins.height,
                       ins.north_vel, ins.east_vel, ins.up_vel,
                       ins.longitudinal_vel, ins.lateral_vel,
                       ins.roll, ins.pitch, ins.heading,
                       ins.latitude_std, ins.longitude_std, ins.height_std,
                       ins.north_vel_std, ins.east_vel_std, ins.up_vel_std,
                       ins.long_vel_std, ins.lat_vel_std,
                       ins.roll_std, ins.pitch_std, ins.heading_std,
                       ins.continent_id);
        if (++count % kPostProcessBatchSize == 0) {
            csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
            fmt_buf.clear();
        }
    }

    if (fmt_buf.size() > 0) {
        csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
    }

    Tool::LogMessage(spdlog::level::info, kModule, fmt::format("    - INS: {} records processed", count));
}


// Read fixed-size binary records written during collection and convert to CSV.
void INSDeviceReceiver::ProcessIMUBinaryFile() {
    std::ifstream bin_in(imu_bin_path_, std::ios::in | std::ios::binary);
    if (!bin_in.is_open()) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("Cannot open IMU binary file: {}", imu_bin_path_));
        return;
    }

    std::ofstream csv_out(imu_csv_path_, std::ios::out);
    if (!csv_out.is_open()) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("Cannot create IMU CSV file: {}", imu_csv_path_));
        return;
    }

    std::vector<char> csv_buffer(write_buffer_size_);
    csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
    fmt::print(csv_out,
               "GPS_Week,GPS_MS[ms],Acc_X[m/s^2],Acc_Y[m/s^2],Acc_Z[m/s^2],"
               "Gyro_X[deg/s],Gyro_Y[deg/s],Gyro_Z[deg/s]\n");

    std::vector<uint8_t> record(RAW_IMU_DATA_LENGTH);
    fmt::memory_buffer fmt_buf;
    std::size_t count = 0;

    while (bin_in.read(reinterpret_cast<char *>(record.data()),
                       static_cast<std::streamsize>(RAW_IMU_DATA_LENGTH))) {
        const auto imu = ParseRawIMUData(record.data());
        fmt::format_to(std::back_inserter(fmt_buf),
                       "{},{},{},{},{},{},{},{}\n",
                       imu.gps_week, imu.gps_millisecs,
                       imu.acc_x, imu.acc_y, imu.acc_z,
                       imu.gyro_x, imu.gyro_y, imu.gyro_z);
        if (++count % kPostProcessBatchSize == 0) {
            csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
            fmt_buf.clear();
        }
    }

    if (fmt_buf.size() > 0) {
        csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
    }

    Tool::LogMessage(spdlog::level::info, kModule, fmt::format("    - IMU: {} records processed", count));
}


// Read fixed-size binary records written during collection and convert to CSV.
void INSDeviceReceiver::ProcessDiagnosticBinaryFile() {
    std::ifstream bin_in(diagnostic_bin_path_, std::ios::in | std::ios::binary);
    if (!bin_in.is_open()) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("Cannot open diagnostic binary file: {}", diagnostic_bin_path_));
        return;
    }

    std::ofstream csv_out(diagnostic_csv_path_, std::ios::out);
    if (!csv_out.is_open()) {
        Tool::LogMessage(spdlog::level::warn, kModule,
                         fmt::format("Cannot create diagnostic CSV file: {}", diagnostic_csv_path_));
        return;
    }

    std::vector<char> csv_buffer(write_buffer_size_);
    csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
    fmt::print(csv_out,
               "GPS_Week,GPS_MS[ms],Device_Status,IMU_Temperature[C],MCU_Temperature[C],GNSS_Chip_Temperature[C]\n");

    std::vector<uint8_t> record(DIAGNOSTIC_MESSAGE_LENGTH);
    fmt::memory_buffer fmt_buf;
    std::size_t count = 0;

    while (bin_in.read(reinterpret_cast<char *>(record.data()),
                       static_cast<std::streamsize>(DIAGNOSTIC_MESSAGE_LENGTH))) {
        const auto diag = ParseDiagnosticMessage(record.data());
        fmt::format_to(std::back_inserter(fmt_buf),
                       "{},{},{},{},{},{}\n",
                       diag.gps_week, diag.gps_millisecs,
                       fmt::format("[{}]", fmt::join(diag.device_status, ",")),
                       diag.imu_temperature, diag.mcu_temperature, diag.gnss_chip_temperature);
        if (++count % kPostProcessBatchSize == 0) {
            csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
            fmt_buf.clear();
        }
    }

    if (fmt_buf.size() > 0) {
        csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
    }

    Tool::LogMessage(spdlog::level::info, kModule, fmt::format("    - Diagnostic: {} records processed", count));
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

    Tool::LogMessage(spdlog::level::info, kModule, fmt::format(
                         "=== INS401 RECEIVER STATISTICS === : Total bytes received: {}",
                         statistic_result.total_bytes_received));
    Tool::LogMessage(spdlog::level::info, kModule, fmt::format(
                         "=== INS401 RECEIVER STATISTICS === : Received GNSS packet: {}; Number of GNSS packet CRC errors {}",
                         statistic_result.gnss_packets, statistic_result.gnss_crc_errors));
    Tool::LogMessage(spdlog::level::info, kModule, fmt::format(
                         "=== INS401 RECEIVER STATISTICS === : Received INS packet: {}; Number of INS packet CRC errors {}",
                         statistic_result.ins_packets, statistic_result.ins_crc_errors));
    Tool::LogMessage(spdlog::level::info, kModule, fmt::format(
                         "=== INS401 RECEIVER STATISTICS === : Received IMU packet: {}; Number of IMU packet CRC errors {}",
                         statistic_result.imu_packets, statistic_result.imu_crc_errors));
    Tool::LogMessage(spdlog::level::info, kModule, fmt::format(
                         "=== INS401 RECEIVER STATISTICS === : Received Diag packet: {}; Number of Diag packet CRC errors {}",
                         statistic_result.diagnostic_packets, statistic_result.diagnostic_crc_errors));
    Tool::LogMessage(spdlog::level::info, kModule, fmt::format(
                         "=== INS401 RECEIVER STATISTICS === : Received RTCM Rover packet: {}; Number of RTCM Rover packet CRC errors {}",
                         statistic_result.rtcm_rover_packets, statistic_result.rtcm_rover_crc_errors));
    Tool::LogMessage(spdlog::level::info, kModule, fmt::format(
                         "=== INS401 RECEIVER STATISTICS === : Received NMEA Rover packet: {}; Number of NMEA Rover packet CRC errors {}",
                         statistic_result.nmea_messages, statistic_result.nmea_checksum_errors));
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
                Tool::LogMessage(level, kModule,
                                 stable_rtk_fixed_
                                     ? "Entered RTK_FIXED position type"
                                     : "Lost RTK_FIXED position type");
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
                Tool::LogMessage(level, kModule,
                                 stable_std_converged_
                                     ? fmt::format("Converged to {:.3f} m STD threshold", rtk_horizontal_std_)
                                     : fmt::format("Horizontal STD diverged above {:.3f} m threshold",
                                                   rtk_horizontal_std_));
            }
        }
    }
}
