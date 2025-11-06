#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <openssl/ssl.h>
#include <queue>
#include <string>
#include <thread>
#include <vector>



class NTRIPClient {
public:
	// Configuration
	struct Config {
		std::string host;
		int port;
		std::string mount_point;
		std::string username;
		std::string password;
		bool is_ssl = true;
		bool verify_ssl = false;
		bool auto_reconnect = true;
		int reconnect_interval = 5;
		int timeout = 10;
		std::string user_agent = "NTRIP/2.0";
		std::string nmea_gga;
	};

	// NTRIP mount point information
	struct MountPoint {
		std::string mount_point;
		std::string city;
		std::string data_format;
		std::string format_details;
		int carrier;
		std::string nav_system;
		std::string network;
		std::string country;
		double latitude;
		double longitude;
		int nmea;
		int solution;
		std::string generator;
		std::string compression;
		std::string authentication;
		int fee;
		int bitrate;
	};

	// Callback for received data
	using DataCallback = std::function<void(const uint8_t *, size_t)>;

	explicit NTRIPClient(Config config);
	~NTRIPClient();

	// Main functions
	bool Connect();
	void Disconnect();
	bool IsConnected() const { return connected_; }

	void StartReceiving();
	void StopReceiving();

	// Get mount points from caster
	std::vector<MountPoint> GetSourceTable();

	// Set callback for data reception
	void SetCallback(DataCallback callback) { callback_ = callback; }

	// Get error message
	std::string GetLastError() const { return last_error_; }

	// Statistics
	size_t GetBytesReceived() const { return bytes_received_; }
	size_t GetMessagesReceived() const { return messages_received_; }
	double GetDataRate() const;

private:
	// Network operations
	bool CreateSocket();
	bool ConnectSocket();
	bool InitSSL();
	bool SendRequest();
	bool ReceiveResponse();
	void CloseConnection();

	// Data operations
	ssize_t SendData(const void *data, size_t size) const;
	ssize_t ReceiveData(void *buffer, size_t size) const;

	// RTCM parsing
	std::vector<std::vector<uint8_t>> ParseRTCM(const uint8_t *data, size_t size);
	static bool ValidateRTCMFrame(const uint8_t *frame, size_t size);

	// Thread functions
	void ReceiveThread();
	void ProcessThread();

	// Reconnection
	void HandleReconnect();

	// Utility
	std::string BuildHTTPRequest(const std::string &path);
	static std::string Base64Encode(const std::string &input);

	// Configuration
	Config config_;

	// Network
	int socket_fd_ = -1;
	ssl_ctx_st *ssl_ctx_ = nullptr;
	ssl_st *ssl_ = nullptr;

	// State
	std::atomic<bool> connected_{ false };
	std::atomic<bool> receiving_{ false };
	std::string last_error_;

	// Threading
	std::unique_ptr<std::thread> receive_thread_;
	std::unique_ptr<std::thread> process_thread_;

	// Data queue
	std::queue<std::vector<uint8_t>> data_queue_;
	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;

	// Callback
	DataCallback callback_;

	// Statistics
	std::atomic<size_t> bytes_received_{ 0 };
	std::atomic<size_t> messages_received_{ 0 };
	std::chrono::steady_clock::time_point start_time_;

	// RTCM buffer
	std::vector<uint8_t> rtcm_buffer_;

	// OpenSSL initialization
	static std::once_flag ssl_init_flag_;
	static void InitOpenSSL();
};


class NTRIP_Callback {
public:
	explicit NTRIP_Callback(const std::string &interface, const std::string &target_mac_str, const std::string &local_mac_str);
	~NTRIP_Callback();

	void SendToINS401(const uint8_t *payload, size_t payload_length);

private:
	int sock_fd_;
	std::string interface_;
	std::array<uint8_t, 6> target_mac_{};
	std::array<uint8_t, 6> local_mac_{};
};
