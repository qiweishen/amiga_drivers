#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// OpenSSL headers for HTTPS support
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "data_type.h"



class NTRIPClient {
public:
	using RTCMCallback = std::function<void(const uint8_t *, size_t)>;
	/**
	 * Constructor for NTRIP Client
	 * @param host NTRIP caster hostname
	 * @param port NTRIP caster port
	 * @param is_ssl Use HTTPS connection if true
	 * @param username Authentication username
	 * @param password Authentication password
	 * @param mountpoint NTRIP mountpoint name
	 */
	NTRIPClient(std::string host, int port, bool is_ssl, std::string username, std::string password, std::string mountpoint);

	~NTRIPClient();

	// Connection management
	bool Connect();

	void Disconnect();

	[[nodiscard]] bool IsConnected() const { return connected_.load(); }

	// Data receiving control
	void StartReceiving();

	void StopReceiving();

	void SetRTCMCallback(RTCMCallback callback) { rtcm_callback_ = std::move(callback); }

	// Configuration methods
	void SetNMEA(const std::string &nmea) { nmea_gga_ = nmea; }
	void SetUserAgent(const std::string &agent) { user_agent_ = agent; }
	void SetConnectionTimeout(int seconds) { connection_timeout_ = seconds; }
	void SetAutoReconnect(bool enable) { auto_reconnect_ = enable; }
	void SetReconnectInterval(int seconds) { reconnect_interval_ = seconds; }
	void SetSSLVerification(bool verify) { verify_ssl_ = verify; }

	// Information retrieval
	std::vector<MountPoint> GetSourceTable();

	[[nodiscard]] std::string GetLastError() const { return last_error_; }

	[[nodiscard]] double GetDataRate() const;

	[[nodiscard]] size_t GetBytesReceived() const { return bytes_received_.load(); }
	[[nodiscard]] size_t GetMessagesReceived() const { return messages_received_.load(); }

private:
	// Socket and SSL members
	int socket_fd_;			   // Socket file descriptor
	SSL_CTX *ssl_ctx_;		   // SSL context
	SSL *ssl_;				   // SSL connection
	bool socket_initialized_;  // Socket initialization flag

	// Connection parameters
	std::string host_;
	int port_;
	bool is_ssl_;
	std::string username_;
	std::string password_;
	std::string mountpoint_;
	std::string user_agent_ = "NTRIP Socket Client/2.0";
	std::string nmea_gga_;

	// Connection state
	std::atomic<bool> connected_{ false };
	std::atomic<bool> receiving_{ false };
	std::string last_error_;

	// Configuration options
	int connection_timeout_ = 10;  // Connection timeout in seconds
	int read_timeout_ = 10;		   // Read timeout in seconds
	int reconnect_interval_ = 5;   // Reconnection interval in seconds
	bool auto_reconnect_ = true;   // Auto reconnection flag
	bool verify_ssl_ = false;	   // SSL certificate verification flag

	// Statistics
	std::atomic<size_t> bytes_received_{ 0 };
	std::atomic<size_t> messages_received_{ 0 };
	std::chrono::steady_clock::time_point start_time_;

	// Data processing
	RTCMCallback rtcm_callback_;
	std::queue<std::vector<uint8_t> > data_queue_;
	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;

	// Thread management
	std::unique_ptr<std::thread> receive_thread_;
	std::unique_ptr<std::thread> process_thread_;

	// Socket operations
	bool InitializeSocket();

	bool ConnectSocket();

	void CloseSocket();

	bool SetSocketTimeout(int timeout_sec);

	// SSL operations
	bool InitializeSSL();

	bool ConnectSSL();

	void CleanupSSL();

	// Network I/O operations
	ssize_t SendData(const void *data, size_t size) const;

	ssize_t ReceiveData(void *buffer, size_t size) const;

	// HTTP protocol handling
	[[nodiscard]] std::string BuildHTTPRequest(const std::string &method, const std::string &path) const;

	[[nodiscard]] std::string BuildAuthHeader() const;

	bool SendHTTPRequest(const std::string &method, const std::string &path);

	bool ReceiveHTTPResponse(HTTPResponse &response);

	bool ParseHTTPResponse(const std::string &data, HTTPResponse &response);

	// RTCM data processing
	static bool ParseRTCMFrame(const std::vector<uint8_t> &buffer, size_t &offset, std::vector<uint8_t> &message);

	// Thread functions
	void ReceiveLoop();

	void ProcessLoop();

	void HandleReconnection();

	// Utility functions
	static MountPoint ParseMountPointLine(const std::string &line);

	static std::string Base64Encode(const std::string &input);

	static std::string Trim(const std::string &str);

	static std::vector<std::string> Split(const std::string &str, char delimiter);

	// Static initialization for SSL
	static void InitializeOpenSSL();

	static void CleanupOpenSSL();

	static std::once_flag openssl_init_flag_;
};
