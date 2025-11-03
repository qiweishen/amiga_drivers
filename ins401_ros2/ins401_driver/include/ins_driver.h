#pragma once

#include <unistd.h>
#include <fstream>
#include <queue>
#include <thread>
#include <condition_variable>

#include "tool.h"
#include "data_type.h"



class EthernetINSReceiver {
public:
    EthernetINSReceiver(std::string iface, const std::string& mac_addr, bool save_to_file = true)
    : sock_fd_(-1), COMMAND_START_(new uint8_t[2]), interface_name_(std::move(iface)),
      running_(true), buffer_(65536), save_to_file_(save_to_file) {
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
            writer_thread_ = std::thread(&EthernetINSReceiver::WriterThread, this);
        }
    }

    ~EthernetINSReceiver() {
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

    void Stop() {
        running_ = false;
    }

    void Run() {
        Initialize();
        ReceiveLoop();
    }

    bool GetGNSSData(std::vector<GNSSSolutionData>& data, size_t max_count = 10);

    bool GetIMUData(std::vector<RawIMUData>& data, size_t max_count = 500);

    bool isRunning() const {
        return running_;
    }


private:
    int sock_fd_{};
    uint8_t *COMMAND_START_;
    std::string interface_name_;
    uint8_t target_mac_[6]{};
    std::atomic<bool> running_;
    std::vector<uint8_t> buffer_;
    std::queue<GNSSSolutionData> gnss_queue_;
    std::queue<RawIMUData> imu_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::thread writer_thread_;
    std::ofstream gnss_file_;
    std::ofstream imu_file_;
    bool save_to_file_;

    bool Initialize();
    void ReceiveLoop();
    void VerifyData(const uint8_t *data, size_t len);
    void ProcessGNSSSolutionData(const uint8_t *packet);
    void ProcessRawIMUData(const uint8_t *packet);
    void WriterThread();
};