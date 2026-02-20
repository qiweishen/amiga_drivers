#ifndef INS_RECEIVER_H
#define INS_RECEIVER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <fstream>
#include <mutex>
#include <queue>
#include <vector>

#include "data_type.h"
#include "ethernet_socket.h"

class NTRIPClient;


// INS401 receiver: writes raw binary during collection, converts to ASCII after stop.
class INSDeviceReceiver {
public:
    explicit INSDeviceReceiver(std::string iface, const std::string &device_mac, std::string output_folder_path,
                               bool check_rtk, bool enable_vrs, double horizontal_std);

    ~INSDeviceReceiver();

    INSDeviceReceiver(const INSDeviceReceiver &) = delete;

    INSDeviceReceiver &operator=(const INSDeviceReceiver &) = delete;

    INSDeviceReceiver(INSDeviceReceiver &&) = delete;

    INSDeviceReceiver &operator=(INSDeviceReceiver &&) = delete;

    void Run();

    void Stop();

    bool isRunning() const { return running_.load(); }

    bool GetGNSSData(std::vector<GNSSSolutionData> &data, std::size_t max_count = 10);

    bool GetIMUData(std::vector<RawIMUData> &data, std::size_t max_count = 500);

    using NmeaCallback = std::function<void(const std::string &)>;
    using ImuCallback = std::function<void(const RawIMUData &)>;
    using GnssCallback = std::function<void(const GNSSSolutionData &)>;

    void SetNmeaCallback(NmeaCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        nmea_callback_ = std::move(callback);
    }

    void SetImuCallback(ImuCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        imu_callback_ = std::move(callback);
    }

    void SetGnssCallback(GnssCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        gnss_callback_ = std::move(callback);
    }

    void SetNtripClient(NTRIPClient *client);

    void ProcessBinaryFiles();

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

        GpsTimeRange imu_time;
        GpsTimeRange ins_time;
        GpsTimeRange gnss_time;
        GpsTimeRange diagnostic_time;

        size_t total_bytes_received = 0;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point last_packet_time;
    };

    Statistics GetStatistics() const;

    void LogStatistics() const;

private:
    std::shared_ptr<EthernetSocket> socket_ptr_;
    std::string interface_name_;
    MacAddress device_mac_{};
    MacAddress local_mac_{};
    std::atomic<bool> running_{false};

    const std::size_t gnss_hz_ = 1;
    const std::size_t imu_hz_ = 100;

    const std::size_t buffer_size_ = 64 * 1024;

    // Queues for real-time public API (GetGNSSData / GetIMUData)
    std::queue<GNSSSolutionData> gnss_queue_;
    const std::size_t max_gnss_queue_size_ = 1 * gnss_hz_ * 60;

    std::queue<RawIMUData> imu_queue_;
    const std::size_t max_imu_queue_size_ = 1 * imu_hz_ * 60;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;

    mutable std::mutex callback_mutex_;
    NmeaCallback nmea_callback_;
    ImuCallback imu_callback_;
    GnssCallback gnss_callback_;

    std::string output_folder_path_;

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

    // CSV file paths
    std::string gnss_csv_path_;
    std::string ins_csv_path_;
    std::string imu_csv_path_;
    std::string diagnostic_csv_path_;

    bool check_rtk_;
    double horizontal_std_;
    static constexpr std::uint8_t GnssTransitionConfirmFrames_ = 3;
    bool gnss_state_initialized_ = false;
    bool stable_rtk_fixed_ = false;
    bool stable_std_converged_ = false;
    bool pending_rtk_fixed_ = false;
    bool pending_std_converged_ = false;
    std::uint8_t pending_rtk_count_ = 0;
    std::uint8_t pending_std_count_ = 0;

    // Statistics tracking
    mutable std::mutex stats_mutex_;
    Statistics stats_{};

    static void UpdateTimeRange(GpsTimeRange &range, std::uint16_t week, std::uint32_t ms, bool first);

    bool enable_vrs_;
    std::mutex first_gga_mutex_;
    std::condition_variable first_gga_cv_;
    std::string first_gga_from_device_;
    std::atomic<bool> first_gga_ready_{false};
    std::atomic<NTRIPClient *> ntrip_client_{nullptr};

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

    void HandleNMEAMessage(const std::uint8_t *packet);

    // Binary-to-struct parsing (shared between real-time callbacks and post-processing)
    static GNSSSolutionData ParseGNSSSolutionData(const std::uint8_t *payload);

    static INSSolutionData ParseINSSolutionData(const std::uint8_t *payload);

    static DiagnosticMessage ParseDiagnosticMessage(const std::uint8_t *payload);

    static RawIMUData ParseRawIMUData(const std::uint8_t *payload);

    // Post-processing: read binary file, write ASCII CSV
    void ProcessGNSSBinaryFile();

    void ProcessINSBinaryFile();

    void ProcessIMUBinaryFile();

    void ProcessDiagnosticBinaryFile();

    // VRS specific functions
    static bool IsGgaSentence(const std::string &nmea);

    void HandleGgaMessage(const std::string &nmea);

    void MonitorGNSSStatus(GNSSSolutionData &gnss);
};


#endif
