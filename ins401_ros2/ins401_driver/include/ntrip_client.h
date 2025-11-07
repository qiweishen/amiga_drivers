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
		std::string host;					   // NTRIP caster hostname
		int port = 2101;					   // Port number
		std::string mount_point;			   // Mount point name
		std::string username;				   // Authentication username
		std::string password;				   // Authentication password
		bool is_ssl = false;				   // Use SSL/TLS connection
		bool verify_ssl = false;			   // Verify SSL certificate
		bool auto_reconnect = true;			   // Enable auto-reconnection
		int reconnect_interval = 5;			   // Reconnection interval in seconds
		int timeout = 10;					   // Socket timeout in seconds
		std::string user_agent = "NTRIP/2.0";  // User agent string
		std::string nmea_gga;				   // NMEA GGA position string

		// Additional safety parameters
		size_t max_buffer_size = 1 * 1024 * 1024;  // Max RTCM buffer size (1MB)
		size_t max_queue_size = 100;			   // Max message queue size
		int max_reconnect_attempts = 10;		   // Maximum reconnection attempts
		bool exponential_backoff = true;		   // Use exponential backoff for reconnection
	};

	// NTRIP mount point information
	struct MountPoint {
		std::string mount_point;
		std::string city;
		std::string data_format;
		std::string format_details;
		int carrier = 0;
		std::string nav_system;
		std::string network;
		std::string country;
		double latitude = 0.0;
		double longitude = 0.0;
		int nmea = 0;
		int solution = 0;
		std::string generator;
		std::string compression;
		std::string authentication;
		int fee = 0;
		int bitrate = 0;
	};

	// Statistics
	struct Statistics {
		size_t bytes_received = 0;		 // Total bytes received
		size_t messages_received = 0;	 // Total messages processed
		size_t messages_dropped = 0;	 // Messages dropped due to queue overflow
		size_t reconnect_count = 0;		 // Number of reconnections
		size_t crc_errors = 0;			 // RTCM CRC errors
		std::chrono::steady_clock::time_point last_message_time;
		double current_data_rate = 0.0;	 // Current data rate in KB/s
	};

	// Callback types
	using DataCallback = std::function<void(const uint8_t *, size_t)>;
	using ErrorCallback = std::function<void(const std::string &)>;
	using MessageCallback = std::function<void(const std::vector<uint8_t> &)>;

	explicit NTRIPClient(Config config);
	~NTRIPClient();
	// Delete copy operations
	NTRIPClient(const NTRIPClient &) = delete;
	NTRIPClient &operator=(const NTRIPClient &) = delete;

	// Main functions
	bool Connect();
	void Disconnect();
	bool IsConnected() const { return connected_.load(std::memory_order_acquire); }

	void StartReceiving();
	void StopReceiving();
	bool IsReceiving() const { return receiving_.load(std::memory_order_acquire); }

	// Get mount points from caster
	std::vector<MountPoint> GetSourceTable();

	// Callback setters
	void SetDataCallback(DataCallback callback) {
		std::lock_guard<std::mutex> lock(callback_mutex_);
		data_callback_ = callback;
	}
	void SetErrorCallback(ErrorCallback callback) {
		std::lock_guard<std::mutex> lock(callback_mutex_);
		error_callback_ = callback;
	}
	void SetMessageCallback(MessageCallback callback) {
		std::lock_guard<std::mutex> lock(callback_mutex_);
		message_callback_ = callback;
	}

	// Get last error message
	std::string GetLastError() const {
		std::lock_guard<std::mutex> lock(error_mutex_);
		return last_error_;
	}

	// Statistics
	Statistics GetStatistics() const;

private:
	// Thread-safe error setter
	void SetLastError(const std::string &error);

	// Network operations
	bool CreateSocket();
	bool ConnectSocket();
	bool InitSSL();
	bool SendRequest();
	bool ReceiveResponse();
	void CloseConnection();

	// Data operations
	ssize_t SendData(const void *data, size_t size);
	ssize_t ReceiveData(void *buffer, size_t size);

	// RTCM parsing
	std::vector<std::vector<uint8_t>> ParseRTCM(const uint8_t *data, size_t size);

	// Thread functions
	void ReceiveThread();
	void ProcessThread();

	// Reconnection
	void HandleReconnect();

	// Utility
	static std::string GetSSLError();
	static std::string GetSSLErrorString(int ssl_error);
	std::string BuildHTTPRequest(const std::string &path);
	static std::string Base64Encode(const std::string &input);

	// Configuration
	Config config_;

	// Network
	int socket_fd_ = -1;
	std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ssl_ctx_{ nullptr, SSL_CTX_free };
	std::unique_ptr<SSL, decltype(&SSL_free)> ssl_{ nullptr, SSL_free };

	// State
	std::atomic<bool> connected_{ false };
	std::atomic<bool> receiving_{ false };

	// Error handling
	mutable std::mutex error_mutex_;
	std::string last_error_;

	// Thread management
	mutable std::mutex connection_mutex_;  // Protects socket and SSL operations
	mutable std::mutex thread_mutex_;	   // Protects thread lifecycle
	std::unique_ptr<std::thread> receive_thread_;
	std::unique_ptr<std::thread> process_thread_;

	// Data queue
	std::queue<std::vector<uint8_t>> data_queue_;
	mutable std::mutex queue_mutex_;
	std::condition_variable queue_cv_;

	// Callbacks
	mutable std::mutex callback_mutex_;
	DataCallback data_callback_;
	ErrorCallback error_callback_;
	MessageCallback message_callback_;

	// Statistics
	mutable std::mutex stats_mutex_;
	Statistics stats_{};

	// RTCM buffer
	std::vector<uint8_t> rtcm_buffer_;
	size_t rtcm_sync_lost_count_ = 0;

	// OpenSSL initialization
	static std::once_flag ssl_init_flag_;
	static void InitOpenSSL();
};


class NTRIP_Callback {
public:
	explicit NTRIP_Callback(const std::string &interface, const std::string &target_mac_str, const std::string &local_mac_str);
	~NTRIP_Callback();
	// Delete copy operations
	NTRIP_Callback(const NTRIP_Callback &) = delete;
	NTRIP_Callback &operator=(const NTRIP_Callback &) = delete;

	bool IsInitialized() const;
	bool SendToINS401(const uint8_t *payload, size_t payload_length);
	size_t GetPacketsSent() const { return packets_sent_.load(std::memory_order_relaxed); }
	size_t GetPacketsFailed() const { return packets_failed_.load(std::memory_order_relaxed); }
	void Reset();

private:
	bool Initialize();
	void Cleanup();

	mutable std::mutex socket_mutex_;
	bool socket_initialized_ = false;

	int sock_fd_ = -1;
	std::string interface_;
	std::array<uint8_t, 6> target_mac_{};
	std::array<uint8_t, 6> local_mac_{};

	// Statistics
	std::atomic<size_t> packets_sent_{ 0 };
	std::atomic<size_t> packets_failed_{ 0 };

	// Retry parameters
	static constexpr int MAX_SEND_RETRIES = 3;
	static constexpr int SEND_RETRY_DELAY_MS = 100;
};
