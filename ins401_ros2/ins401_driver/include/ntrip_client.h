#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>



// RTCM数据回调函数类型
using RTCMCallback = std::function<void(const std::vector<uint8_t>& data, size_t length)>;


// Mountpoint信息结构
struct MountPoint {
    std::string name;
    std::string identifier;
    std::string format;
    std::string nav_system;
    std::string network;
    std::string country;
    double latitude;
    double longitude;
    int carrier;
    std::string solution;
    std::string generator;
    std::string compression;
    std::string authentication;
    int fee;
    int bitrate;
};


class NTRIPClient {
public:
    NTRIPClient(std::string &host, int port,
                         std::string &username,
                         std::string &password,
                         std::string &mountpoint);
    ~NTRIPClient();

    // 连接控制
    bool Connect();
    void Disconnect();
    bool isConnected() const;

    // 获取可用的挂载点列表
    std::vector<MountPoint> GetSourceTable();

    // 数据接收
    void SetRTCMCallback(RTCMCallback callback);
    void StartReceiving();
    void StopReceiving();

    // 连接状态和统计
    double GetDataRate() const; // KB/s

    // 设置超时和重连参数
    void SetConnectionTimeout(int seconds);
    void SetReceiveTimeout(int seconds);
    void SetAutoReconnect(bool enable);
    void SetReconnectInterval(int seconds);


private:
    int socket_fd_;
    std::string host_;
    int port_;
    std::string username_;
    std::string password_;
    std::string mountpoint_;
    std::string user_agent_;
    std::string gga_string_;

    std::atomic<bool> connected_;
    std::atomic<bool> receiving_;
    std::string last_error_;

    std::unique_ptr<std::thread> receive_thread_;
    std::unique_ptr<std::thread> process_thread_;

    std::queue<std::vector<uint8_t>> data_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    RTCMCallback rtcm_callback_;

    std::atomic<size_t> bytes_received_;
    std::atomic<size_t> messages_received_;
    std::chrono::steady_clock::time_point start_time_;

    int connection_timeout_;
    int receive_timeout_;
    bool auto_reconnect_;
    int reconnect_interval_;

    bool CreateSocket();
    bool SendNTRIPRequest();
    bool ParseNTRIPResponse();
    void ReceiveLoop();
    void ProcessLoop();
    bool ParseRTCMFrame(const std::vector<uint8_t>& buffer, size_t& offset, std::vector<uint8_t>& message);
    void HandleReconnection();
    std::string EncodeBase64(const std::string& input);
};
