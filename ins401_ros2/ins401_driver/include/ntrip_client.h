#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// OpenSSL forward declarations
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

// ==================== NTRIP Client - Simplified & Clear ====================

class NTRIPClient {
public:
	// ---------- Configuration ----------
	struct Config {
		// Connection
		std::string host;
		int port = 2101;
		std::string mountpoint;

		// Authentication
		std::string username;
		std::string password;

		// SSL/TLS
		bool use_ssl = false;
		bool verify_ssl = true;

		// Behavior
		bool auto_reconnect = true;
		int reconnect_interval = 5;	  // seconds
		int connection_timeout = 10;  // seconds
		int receive_timeout = 30;	  // seconds

		// Performance
		int receive_buffer_size = 256 * 1024;  // 256KB
		bool use_nonblocking = false;

		// Protocol
		std::string user_agent = "NTRIP Client/2.0";
		std::string nmea_gga;  // Optional NMEA GGA string for VRS
	};

	// ---------- Mount Point Info ----------
	struct MountPoint {
		std::string mountpoint;
		std::string city;
		std::string data_format;
		std::string format_details;
		int carrier = 0;
		std::string nav_system;
		std::string network;
		std::string country;
		double latitude = 0.0;
		double longitude = 0.0;
	};

	// ---------- Statistics ----------
	struct Statistics {
		std::chrono::steady_clock::time_point connect_time;
		size_t bytes_received = 0;
		size_t messages_received = 0;
		size_t crc_errors = 0;
		size_t reconnect_count = 0;

		double GetDataRate() const;	 // KB/s
		std::chrono::seconds GetUptime() const;
		void Reset();
	};

	// ---------- Callbacks ----------
	using DataCallback = std::function<void(const uint8_t*, size_t)>;
	using ErrorCallback = std::function<void(const std::string&)>;
	using StatusCallback = std::function<void(const std::string&)>;

	// ---------- Constructor & Destructor ----------
	explicit NTRIPClient(const Config& config);
	~NTRIPClient();

	// Disable copy
	NTRIPClient(const NTRIPClient&) = delete;
	NTRIPClient& operator=(const NTRIPClient&) = delete;

	// ---------- Connection Management ----------
	bool Connect();
	void Disconnect();
	bool IsConnected() const { return connected_.load(); }

	// ---------- Data Streaming ----------
	void StartReceiving();
	void StopReceiving();
	bool IsReceiving() const { return receiving_.load(); }

	// ---------- Source Table ----------
	std::vector<MountPoint> GetSourceTable();

	// ---------- Configuration ----------
	void SetConfig(const Config& config);
	Config GetConfig() const;

	// ---------- Callbacks ----------
	void SetDataCallback(DataCallback cb) { data_callback_ = cb; }
	void SetErrorCallback(ErrorCallback cb) { error_callback_ = cb; }
	void SetStatusCallback(StatusCallback cb) { status_callback_ = cb; }

	// ---------- Information ----------
	Statistics GetStatistics() const { return stats_; }
	std::string GetLastError() const { return last_error_; }

	// ---------- NMEA GGA Update (for VRS) ----------
	void UpdateNMEA(const std::string& gga);

private:
	// ========== Internal Classes ==========

	// ---------- Socket Manager (RAII) ----------
	class SocketManager {
	public:
		SocketManager();
		~SocketManager();

		bool Create();
		bool Configure(const Config& config);
		bool Connect(const std::string& host, int port, int timeout_sec);
		void Close();

		ssize_t Send(const void* data, size_t size);
		ssize_t Receive(void* buffer, size_t size);

		int GetFD() const { return fd_; }
		bool IsValid() const { return fd_ >= 0; }

	private:
		int fd_ = -1;
		bool SetSocketOptions(const Config& config);
		bool SetTimeouts(int timeout_sec);
		bool SetNonBlocking(bool enable);
	};

	// ---------- SSL Manager (RAII) ----------
	class SSLManager {
	public:
		SSLManager();
		~SSLManager();

		bool Initialize(int socket_fd, bool verify_ssl);
		bool Connect();
		void Shutdown();

		ssize_t Send(const void* data, size_t size);
		ssize_t Receive(void* buffer, size_t size);

		bool IsValid() const { return ssl_ != nullptr; }

	private:
		SSL_CTX* ctx_ = nullptr;
		SSL* ssl_ = nullptr;
		static std::once_flag openssl_init_flag_;
		static void InitializeOpenSSL();
	};

	// ---------- HTTP Protocol Handler ----------
	class HTTPHandler {
	public:
		struct Response {
			int status_code = 0;
			std::string status_text;
			std::map<std::string, std::string> headers;
			std::string body;
		};

		static std::string BuildRequest(const Config& config, const std::string& method, const std::string& path);
		static Response ParseResponse(const std::string& data);
		static std::string EncodeBase64(const std::string& input);

	private:
		static std::string BuildAuthHeader(const Config& config);
	};

	// ---------- RTCM Parser ----------
	class RTCMParser {
	public:
		struct Message {
			uint16_t type;
			std::vector<uint8_t> data;
		};

		RTCMParser();
		void Reset();
		std::vector<Message> Parse(const uint8_t* data, size_t size);

	private:
		std::vector<uint8_t> buffer_;

		bool FindFrame(size_t& offset, std::vector<uint8_t>& frame);
		bool ValidateCRC(const std::vector<uint8_t>& frame);
		uint16_t ExtractMessageType(const std::vector<uint8_t>& frame);

		static constexpr uint8_t PREAMBLE = 0xD3;
		static constexpr size_t MAX_MESSAGE_SIZE = 1023;
	};

	// ---------- Data Buffer ----------
	struct DataPacket {
		std::vector<uint8_t> data;
		std::chrono::steady_clock::time_point timestamp;
	};

	// ========== Private Methods ==========

	// Connection
	bool ConnectInternal();
	bool PerformHandshake();
	bool SendHTTPRequest();
	bool ReceiveHTTPResponse();
	void HandleReconnection();

	// Data handling
	void ReceiveThread();
	void ProcessThread();
	ssize_t SendData(const void* data, size_t size);
	ssize_t ReceiveData(void* buffer, size_t size);

	// Utility
	void ReportError(const std::string& error);
	void ReportStatus(const std::string& status);
	static std::string Trim(const std::string& str);
	static std::vector<std::string> Split(const std::string& str, char delimiter);
	static MountPoint ParseMountPoint(const std::string& line);

	// ========== Member Variables ==========

	// Configuration
	Config config_;
	mutable std::mutex config_mutex_;

	// Connection
	std::unique_ptr<SocketManager> socket_;
	std::unique_ptr<SSLManager> ssl_;
	std::atomic<bool> connected_{ false };
	std::atomic<bool> receiving_{ false };

	// Threading
	std::unique_ptr<std::thread> receive_thread_;
	std::unique_ptr<std::thread> process_thread_;

	// Data queue
	std::queue<DataPacket> data_queue_;
	mutable std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	static constexpr size_t MAX_QUEUE_SIZE = 100;

	// RTCM Parser
	std::unique_ptr<RTCMParser> parser_;

	// Callbacks
	DataCallback data_callback_;
	ErrorCallback error_callback_;
	StatusCallback status_callback_;

	// Statistics & State
	Statistics stats_;
	mutable std::string last_error_;

	// Reconnection
	std::atomic<int> reconnect_attempts_{ 0 };
	std::chrono::steady_clock::time_point last_reconnect_time_;
};
