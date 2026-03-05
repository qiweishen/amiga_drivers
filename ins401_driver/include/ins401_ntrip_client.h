/// @file ntrip_client.h
/// @brief NTRIP v2.0 client for receiving RTK correction data (RTCM3) and forwarding it to INS401.

#ifndef NTRIP_CLIENT_H
#define NTRIP_CLIENT_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <openssl/ssl.h>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ins401_data_type.h"
#include "ins401_ethernet_socket.h"


// HTTP response structure
struct HTTPResponse {
	int status_code;
	std::string status_text;
	std::map<std::string, std::string> headers;
	std::string body;
	bool is_chunked;
};


class NTRIPClient {
public:
	struct Options {
		bool enable_rtk;
		std::string host;		  // NTRIP caster hostname
		int port;				  // Port number
		std::string mount_point;  // Mount point name
		std::string username;	  // Authentication username
		std::string password;	  // Authentication password
		bool is_ssl = false;	  // Use SSL/TLS connection
		bool verify_ssl = false;  // Verify SSL certificate
		bool use_vrs;			  // Enable periodic GGA for VRS
		int gga_interval = 30;	  // GGA send interval in seconds

		// Connection parameters
		bool auto_reconnect = true;			   // Enable auto-reconnection
		int reconnect_interval = 5;			   // Reconnection interval in seconds
		int timeout = 1;					   // Socket timeout in seconds
		std::string user_agent = "NTRIP/2.0";  // User agent string

		// Additional safety parameters
		size_t max_buffer_size = 8 * 1024 * 1024;  // Max RTCM buffer size (8MB)
		size_t max_queue_size = 1024;			   // Max message queue size
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

	explicit NTRIPClient(const INSConfig &configures);

	~NTRIPClient();

	// Delete copy operations
	NTRIPClient(const NTRIPClient &) = delete;

	NTRIPClient &operator=(const NTRIPClient &) = delete;

	// Callback types
	using DataCallback = std::function<void(const uint8_t *, size_t)>;
	using MessageCallback = std::function<void(const std::vector<uint8_t> &)>;

	bool Connect();

	void Disconnect();

	[[nodiscard]] bool IsConnected() const { return connected_.load(std::memory_order_acquire); }

	void StartReceiving();

	[[nodiscard]] bool IsReceiving() const { return receiving_.load(std::memory_order_acquire); }

	[[nodiscard]] std::vector<MountPoint> GetSourceTable();

	[[nodiscard]] bool IsRTKRequired() const;

	// Callback setters
	void SetDataCallback(DataCallback callback) {
		std::scoped_lock lock(callback_mutex_);
		data_callback_ = std::move(callback);
	}

	void SetMessageCallback(MessageCallback callback) {
		std::scoped_lock lock(callback_mutex_);
		message_callback_ = std::move(callback);
	}

	void SetNmeaGga(std::string gga) {
		std::scoped_lock lock(gga_mutex_);
		nmea_gga_ = std::move(gga);
	}

	// Statistics
	Statistics GetStatistics() const;

private:
	void StopReceiving();

	// Loading config
	void LoadConfig(const INSConfig &config);

	// Network operations
	bool CreateSocket(int family);

	bool ConnectSocket();

	bool InitSSL();

	bool SendRequest() const;

	bool ReceiveResponse();

	void CloseConnection();

	void SendGGA();

	// Data operations
	ssize_t SendData(const void *data, size_t size) const;

	ssize_t ReceiveData(void *buffer, size_t size) const;

	// Split raw RTCM stream into 1024-byte blocks that fit within the Ethernet MTU
	// for raw socket transmission to the INS401 device.
	std::vector<std::vector<uint8_t> > ChunkRTCMData(const uint8_t *data, size_t size);

	// Thread functions
	void ReceiveThread();

	void ProcessThread();

	// Reconnection
	void HandleReconnect();

	// Utility
	static std::string GetSSLError();

	static std::string GetSSLErrorString(int ssl_error);

	std::string BuildHTTPRequest(const std::string &path) const;

	static std::string Base64Encode(const std::string &input);

	// Log error and throw if RTK is required, otherwise log warning and continue
	void LogErrorOrWarn(std::string_view msg) const;

	// Configuration
	Options config_{};

	// Network
	int socket_fd_ = -1;
	std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ssl_ctx_{ nullptr, SSL_CTX_free };
	std::unique_ptr<SSL, decltype(&SSL_free)> ssl_{ nullptr, SSL_free };

	// State
	std::atomic<bool> connected_{ false };
	std::atomic<bool> disconnected_{ false };
	std::atomic<bool> receiving_{ false };

	// Thread management
	mutable std::mutex connection_mutex_;  // Protects socket and SSL operations
	mutable std::mutex thread_mutex_;	   // Protects thread lifecycle
	std::unique_ptr<std::thread> receive_thread_;
	std::unique_ptr<std::thread> process_thread_;

	// Data queue
	std::queue<std::vector<uint8_t> > data_queue_;
	mutable std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::vector<uint8_t> pending_data_;

	// VRS GGA
	mutable std::mutex gga_mutex_;
	std::string nmea_gga_;
	std::chrono::steady_clock::time_point last_gga_sent_;

	// Callbacks
	mutable std::mutex callback_mutex_;
	DataCallback data_callback_;
	MessageCallback message_callback_;

	// Statistics
	mutable std::mutex stats_mutex_;
	Statistics stats_{};

	// RTCM buffer
	std::vector<uint8_t> rtcm_buffer_;
	size_t rtcm_sync_lost_count_ = 0;

	// RTCM base stream recording (for PPK)
	std::string output_folder_path_;
	std::ofstream rtcm_base_file_;
	std::array<char, 256 * 1024> rtcm_base_file_buffer_{};

	// OpenSSL initialization
	static std::once_flag ssl_init_flag_;

	static void InitOpenSSL();
};


class NTRIPCallback {
public:
	explicit NTRIPCallback(std::string interface, std::string target_mac_str, std::string local_mac_str);

	~NTRIPCallback();

	// Delete copy operations
	NTRIPCallback(const NTRIPCallback &) = delete;

	NTRIPCallback &operator=(const NTRIPCallback &) = delete;

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

	std::shared_ptr<EthernetSocket> socket_ptr_;
	std::string interface_;
	std::array<uint8_t, 6> target_mac_{};
	std::array<uint8_t, 6> local_mac_{};

	// Statistics
	std::atomic<size_t> packets_sent_{ 0 };
	std::atomic<size_t> packets_failed_{ 0 };

	static constexpr int kMaxSendRetries = 3;
	static constexpr int kSendRetryDelayMs = 10;
};


#endif
