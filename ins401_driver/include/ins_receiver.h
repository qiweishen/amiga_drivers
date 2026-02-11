#ifndef INS_RECEIVER_H
#define INS_RECEIVER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <fstream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "data_type.h"
#include "ethernet_socket.h"

class NTRIPClient;


// INS401 receiver with queues and optional file logging.
class INSDeviceReceiver {
public:
    explicit INSDeviceReceiver(std::string iface, const std::string &device_mac, bool save_to_file,
                               std::string output_folder_path, double horizontal_std, bool enable_vrs=false);

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

private:
    std::shared_ptr<EthernetSocket> socket_ptr_;
    std::string interface_name_;
    MacAddress device_mac_{};
    MacAddress local_mac_{};
    std::atomic<bool> running_{false};

    const std::size_t gnss_hz_ = 1;
    const std::size_t ins_hz_ = 100;
    const std::size_t diagnostic_hz_ = 1;
    const std::size_t imu_hz_ = 100;
    const std::size_t rtcm_rover_hz_ = 10;

    const std::size_t buffer_size_ = 64 * 1024;

    std::queue<GNSSSolutionData> gnss_queue_;
    const std::size_t max_gnss_queue_size_ = 1 * gnss_hz_ * 60;

    std::queue<INSSolutionData> ins_queue_;
    const std::size_t max_ins_queue_size_ = 1 * ins_hz_ * 60;

    std::queue<DiagnosticMessage> diagnostic_queue_;
    const std::size_t max_diagnostic_queue_size_ = 1 * diagnostic_hz_ * 60;

    std::queue<RawIMUData> imu_queue_;
    const std::size_t max_imu_queue_size_ = 1 * imu_hz_ * 60;

    std::queue<std::vector<std::uint8_t> > rtcm_rover_queue_;
    const std::size_t max_rtcm_rover_queue_size_ = 1 * rtcm_rover_hz_ * 60;

    std::queue<std::string> nmea_queue_;
    const std::size_t max_nmea_queue_size_ = 128;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;

public:
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

    // std::string WaitForFirstGga(const std::atomic<bool> *terminate_flag = nullptr,
    //                             std::chrono::milliseconds wait_step = std::chrono::milliseconds(200));

private:
    mutable std::mutex callback_mutex_;
    NmeaCallback nmea_callback_;
    ImuCallback imu_callback_;
    GnssCallback gnss_callback_;

    bool save_to_file_;
    std::string output_folder_path_;
    std::thread writer_thread_;

    std::ofstream gnss_file_;
    std::ofstream ins_file_;
    std::ofstream diagnostic_file_;
    std::ofstream imu_file_;
    std::ofstream rtcm_rover_file_;
    std::ofstream nmea_file_;

    const std::size_t gnss_write_batch_size_ = gnss_hz_ * 10;
    const std::size_t ins_write_batch_size_ = ins_hz_ * 10;
    const std::size_t diagnostic_write_batch_size_ = diagnostic_hz_ * 10;
    const std::size_t imu_write_batch_size_ = imu_hz_ * 10;
    const std::size_t rtcm_rover_write_batch_size_ = rtcm_rover_hz_ * 64;
    const std::size_t nmea_write_batch_size_ = 24;
    const std::size_t write_buffer_size_ = 256 * 1024;

    std::vector<char> gnss_file_buffer_;
    std::vector<char> ins_file_buffer_;
    std::vector<char> diagnostic_file_buffer_;
    std::vector<char> imu_file_buffer_;
    std::vector<char> rtcm_rover_file_buffer_;
    std::vector<char> nmea_file_buffer_;
    std::size_t last_flush_time_ = 0;

    double horizontal_std_;

    bool enable_vrs_;
    std::mutex first_gga_mutex_;
    std::condition_variable first_gga_cv_;
    std::string first_gga_from_device_;
    std::atomic<bool> first_gga_ready_{false};
    std::atomic<NTRIPClient *> ntrip_client_{nullptr};

    bool InitializeWritingFiles();

    void ReceiveLoop();

    void VerifyDataFrame(const std::uint8_t *data, std::size_t len);

    void ProcessGNSSSolutionData(const std::uint8_t *packet);

    void ProcessINSSolutionData(const std::uint8_t *packet);

    void ProcessDiagnosticMessage(const std::uint8_t *packet);

    void ProcessRawIMUData(const std::uint8_t *packet);

    void ProcessRTCMRoverData(const std::uint8_t *packet, std::size_t len);

    void ProcessNMEAMessage(const std::uint8_t *packet);

    static bool IsGgaSentence(const std::string &nmea);

    void HandleGgaMessage(const std::string &nmea);

    void WriterThread();

    void WriteGNSSBatch(const std::vector<GNSSSolutionData> &batch);

    void WriteINSBatch(const std::vector<INSSolutionData> &batch);

    void WriteDiagnosticBatch(const std::vector<DiagnosticMessage> &batch);

    void WriteIMUBatch(const std::vector<RawIMUData> &batch);

    void WriteRTCMRoverBatch(const std::vector<std::vector<std::uint8_t> > &batch);

    void WriteNMEABatch(const std::vector<std::string> &batch);
};


#endif