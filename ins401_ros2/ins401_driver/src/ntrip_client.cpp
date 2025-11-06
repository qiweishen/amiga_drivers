#include "ntrip_client.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>



std::once_flag NTRIPClient::ssl_init_flag_;


NTRIPClient::NTRIPClient(const Config& config) : config_(config) {
	rtcm_buffer_.reserve(8 * 1024);
	std::call_once(ssl_init_flag_, InitOpenSSL);
}


NTRIPClient::~NTRIPClient() {
	Disconnect();
}


void NTRIPClient::InitOpenSSL() {
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
}


bool NTRIPClient::Connect() {
	if (connected_) {
		return true;
	}
	// Create and connect socket
	if (!CreateSocket() || !ConnectSocket()) {
		CloseConnection();
		return false;
	}
	// Initialize SSL if needed
	if (config_.use_ssl && !InitSSL()) {
		CloseConnection();
		return false;
	}
	// Send NTRIP request
	if (!SendRequest()) {
		CloseConnection();
		return false;
	}
	// Receive and check response
	if (!ReceiveResponse()) {
		CloseConnection();
		return false;
	}

	connected_ = true;
	start_time_ = std::chrono::steady_clock::now();
	bytes_received_ = 0;
	messages_received_ = 0;
	return true;
}


void NTRIPClient::Disconnect() {
	StopReceiving();
	connected_ = false;
	CloseConnection();
}

void NTRIPClient::StartReceiving() {
	if (!connected_ || receiving_) {
		return;
	}

	receiving_ = true;
	receive_thread_ = std::make_unique<std::thread>(&NTRIPClient::ReceiveThread, this);
	process_thread_ = std::make_unique<std::thread>(&NTRIPClient::ProcessThread, this);
}

void NTRIPClient::StopReceiving() {
	receiving_ = false;
	queue_cv_.notify_all();

	if (receive_thread_ && receive_thread_->joinable()) {
		receive_thread_->join();
		receive_thread_.reset();
	}

	if (process_thread_ && process_thread_->joinable()) {
		process_thread_->join();
		process_thread_.reset();
	}
}

std::vector<NTRIPClient::MountPoint> NTRIPClient::GetSourceTable() {
	std::vector<MountPoint> result;

	// Temporarily connect to get source table
	if (!CreateSocket() || !ConnectSocket()) {
		return result;
	}

	if (config_.use_ssl && !InitSSL()) {
		CloseConnection();
		return result;
	}

	// Send request for source table (empty path)
	std::string request = BuildHTTPRequest("/");
	if (SendData(request.c_str(), request.size()) <= 0) {
		CloseConnection();
		return result;
	}

	// Receive response
	char buffer[4096];
	std::string response;
	while (true) {
		ssize_t received = ReceiveData(buffer, sizeof(buffer) - 1);
		if (received <= 0)
			break;
		buffer[received] = '\0';
		response += buffer;
	}

	// Parse source table
	std::istringstream stream(response);
	std::string line;
	while (std::getline(stream, line)) {
		if (line.find("STR;") == 0) {
			// Parse mount point line
			MountPoint mp;
			std::vector<std::string> fields;
			std::stringstream ss(line);
			std::string field;

			while (std::getline(ss, field, ';')) {
				fields.push_back(field);
			}

			if (fields.size() > 1)
				mp.mountpoint = fields[1];
			if (fields.size() > 2)
				mp.city = fields[2];
			if (fields.size() > 3)
				mp.format = fields[3];
			if (fields.size() > 4)
				mp.details = fields[4];
			if (fields.size() > 6)
				mp.nav_system = fields[6];
			if (fields.size() > 8)
				mp.country = fields[8];
			if (fields.size() > 9) {
				try {
					mp.latitude = std::stod(fields[9]);
				} catch (...) {
				}
			}
			if (fields.size() > 10) {
				try {
					mp.longitude = std::stod(fields[10]);
				} catch (...) {
				}
			}

			if (!mp.mountpoint.empty()) {
				result.push_back(mp);
			}
		}
	}

	CloseConnection();
	return result;
}

double NTRIPClient::GetDataRate() const {
	if (!connected_)
		return 0.0;
	auto now = std::chrono::steady_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
	if (duration == 0)
		return 0.0;
	return static_cast<double>(bytes_received_.load()) / duration / 1024.0;	 // KB/s
}


bool NTRIPClient::CreateSocket() {
	socket_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_fd_ < 0) {
		socket_fd_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
		if (socket_fd_ < 0) {
			last_error_ = "Failed to create socket: " + std::string(strerror(errno));
			return false;
		}
	}

	// RAII guard for automatic cleanup on failure
	struct SocketGuard {
		int& fd;
		bool released = false;
		explicit SocketGuard(int& socket_fd) : fd(socket_fd) {}
		~SocketGuard() {
			if (!released && fd >= 0) {
				close(fd);
				fd = -1;
			}
		}
		void release() { released = true; }
	} guard(socket_fd_);

	// Set socket options
	constexpr int reuse = 1;
	if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		last_error_ = "Failed to set SO_REUSEADDR: " + std::string(strerror(errno));
		return false;
	}
	// TCP_NODELAY for low latency
	constexpr int nodelay = 1;
	if (setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
		last_error_ = "Failed to set TCP_NODELAY: " + std::string(strerror(errno));
		return false;
	}

	// Set socket timeout
	timeval tv{};
	tv.tv_sec = config_.timeout;
	tv.tv_usec = 0;
	if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		last_error_ = "Failed to set receive timeout: " + std::string(strerror(errno));
		return false;
	}
	if (setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
		last_error_ = "Failed to set send timeout: " + std::string(strerror(errno));
		return false;
	}

	// Set keep-alive
	constexpr int keepalive = 1;
	if (setsockopt(socket_fd_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
		last_error_ = "Failed to set keep-alive" + std::string(strerror(errno));
		return false;
	}

	// Set non-blocking mode
	int flags = fcntl(socket_fd_, F_GETFL, 0);
	if (flags < 0 || fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
		last_error_ = "Failed to set non-blocking mode: " + std::string(strerror(errno));
		return false;
	}

	guard.release();
	return true;
}


bool NTRIPClient::ConnectSocket() {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo* result = nullptr;
	std::string port_str = std::to_string(config_.port);
	int ret = getaddrinfo(config_.host.c_str(), port_str.c_str(), &hints, &result);
	if (ret != 0) {
		last_error_ = "Failed to resolve hostname '" + config_.host + "': " + gai_strerror(ret);
		return false;
	}

	// RAII for addrinfo cleanup
	struct AddrInfoGuard {
		addrinfo* info;
		explicit AddrInfoGuard(addrinfo* ai) : info(ai) {}
		~AddrInfoGuard() {
			if (info)
				freeaddrinfo(info);
		}
	} addr_guard(result);

	// 尝试连接到所有返回的地址，直到成功
	// TODO
	if (!connected_) {
		if (socket_fd_ >= 0) {
			close(socket_fd_);
			socket_fd_ = -1;
		}
		last_error_ = "Failed to connect to any address for " + config_.host + ". Errors: " + connect_errors;
		return false;
	}
	return true;
}


bool NTRIPClient::InitSSL() {
	const SSL_METHOD* method = TLS_client_method();
	ssl_ctx_ = SSL_CTX_new(method);
	if (!ssl_ctx_) {
		last_error_ = "Failed to create SSL context";
		return false;
	}

	if (!config_.verify_ssl) {
		SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
	}

	ssl_ = SSL_new(ssl_ctx_);
	if (!ssl_) {
		last_error_ = "Failed to create SSL connection";
		return false;
	}

	SSL_set_fd(ssl_, socket_fd_);

	if (SSL_connect(ssl_) <= 0) {
		last_error_ = "SSL handshake failed";
		return false;
	}

	return true;
}

bool NTRIPClient::SendRequest() {
	std::string request = BuildHTTPRequest("/" + config_.mount_point);
	size_t total_sent = 0;

	while (total_sent < request.size()) {
		ssize_t sent = SendData(request.c_str() + total_sent, request.size() - total_sent);
		if (sent <= 0) {
			last_error_ = "Failed to send request";
			return false;
		}
		total_sent += sent;
	}

	return true;
}

bool NTRIPClient::ReceiveResponse() {
	char buffer[1024];
	std::string response;

	// Read until we have complete headers
	while (response.find("\r\n\r\n") == std::string::npos) {
		ssize_t received = ReceiveData(buffer, sizeof(buffer) - 1);
		if (received <= 0) {
			last_error_ = "Failed to receive response";
			return false;
		}
		buffer[received] = '\0';
		response += buffer;
	}

	// Parse status code
	if (response.find("200 OK") == std::string::npos) {
		if (response.find("401") != std::string::npos) {
			last_error_ = "Authentication failed";
		} else if (response.find("404") != std::string::npos) {
			last_error_ = "Mount point not found: " + config_.mount_point;
		} else {
			last_error_ = "HTTP error response";
		}
		return false;
	}

	return true;
}

void NTRIPClient::CloseConnection() {
	if (ssl_) {
		SSL_shutdown(ssl_);
		SSL_free(ssl_);
		ssl_ = nullptr;
	}

	if (ssl_ctx_) {
		SSL_CTX_free(ssl_ctx_);
		ssl_ctx_ = nullptr;
	}

	if (socket_fd_ >= 0) {
		close(socket_fd_);
		socket_fd_ = -1;
	}
}

ssize_t NTRIPClient::SendData(const void* data, size_t size) {
	if (ssl_) {
		return SSL_write(ssl_, data, size);
	}
	return send(socket_fd_, data, size, MSG_NOSIGNAL);
}

ssize_t NTRIPClient::ReceiveData(void* buffer, size_t size) {
	if (ssl_) {
		return SSL_read(ssl_, buffer, size);
	}
	return recv(socket_fd_, buffer, size, 0);
}

// ==================== RTCM Parsing ====================

std::vector<std::vector<uint8_t>> NTRIPClient::ParseRTCM(const uint8_t* data, size_t size) {
	std::vector<std::vector<uint8_t>> messages;

	// Add to buffer
	rtcm_buffer_.insert(rtcm_buffer_.end(), data, data + size);

	size_t offset = 0;
	while (offset < rtcm_buffer_.size()) {
		// Find RTCM3 preamble (0xD3)
		while (offset < rtcm_buffer_.size() && rtcm_buffer_[offset] != 0xD3) {
			offset++;
		}

		if (offset >= rtcm_buffer_.size())
			break;

		// Need at least 3 bytes for header
		if (offset + 3 > rtcm_buffer_.size())
			break;

		// Get message length
		uint16_t length = ((rtcm_buffer_[offset + 1] & 0x03) << 8) | rtcm_buffer_[offset + 2];

		// Check for complete frame
		size_t frame_size = 3 + length + 3;	 // header + payload + CRC
		if (offset + frame_size > rtcm_buffer_.size())
			break;

		// Validate frame
		if (ValidateRTCMFrame(&rtcm_buffer_[offset], frame_size)) {
			std::vector<uint8_t> message(rtcm_buffer_.begin() + offset, rtcm_buffer_.begin() + offset + frame_size);
			messages.push_back(message);
			messages_received_++;
		}

		offset += frame_size;
	}

	// Remove processed data
	if (offset > 0) {
		rtcm_buffer_.erase(rtcm_buffer_.begin(), rtcm_buffer_.begin() + offset);
	}

	// Prevent buffer overflow
	if (rtcm_buffer_.size() > 16384) {
		rtcm_buffer_.clear();
	}

	return messages;
}

bool NTRIPClient::ValidateRTCMFrame(const uint8_t* frame, size_t size) {
	if (size < 6)
		return false;

	size_t length = size - 3;
	uint32_t computed = CalculateCRC24(frame, length);
	uint32_t received = (frame[length] << 16) | (frame[length + 1] << 8) | frame[length + 2];

	return computed == received;
}

uint32_t NTRIPClient::CalculateCRC24(const uint8_t* data, size_t length) {
	uint32_t crc = 0;
	for (size_t i = 0; i < length; i++) {
		crc = ((crc << 8) ^ CRC24_TABLE[((crc >> 16) ^ data[i]) & 0xFF]) & 0xFFFFFF;
	}
	return crc;
}

// ==================== Thread Functions ====================

void NTRIPClient::ReceiveThread() {
	uint8_t buffer[4096];

	while (receiving_ && connected_) {
		ssize_t received = ReceiveData(buffer, sizeof(buffer));

		if (received > 0) {
			bytes_received_ += received;

			// Add to queue
			std::vector<uint8_t> data(buffer, buffer + received);
			{
				std::lock_guard<std::mutex> lock(queue_mutex_);
				data_queue_.push(std::move(data));
			}
			queue_cv_.notify_one();

		} else if (received == 0) {
			// Connection closed
			connected_ = false;
			if (config_.auto_reconnect && receiving_) {
				HandleReconnect();
			}
		} else {
			// Error
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT) {
				connected_ = false;
				if (config_.auto_reconnect && receiving_) {
					HandleReconnect();
				}
			}
		}
	}
}

void NTRIPClient::ProcessThread() {
	while (receiving_) {
		std::unique_lock<std::mutex> lock(queue_mutex_);
		queue_cv_.wait(lock, [this] { return !data_queue_.empty() || !receiving_; });

		while (!data_queue_.empty() && receiving_) {
			auto data = std::move(data_queue_.front());
			data_queue_.pop();
			lock.unlock();

			// Parse RTCM messages
			auto messages = ParseRTCM(data.data(), data.size());

			// Deliver via callback
			if (callback_) {
				for (const auto& msg: messages) {
					callback_(msg.data(), msg.size());
				}
			}

			lock.lock();
		}
	}
}

void NTRIPClient::HandleReconnect() {
	int attempts = 0;

	while (config_.auto_reconnect && receiving_) {
		std::this_thread::sleep_for(std::chrono::seconds(config_.reconnect_interval));

		attempts++;
		std::cout << "Reconnection attempt #" << attempts << std::endl;

		CloseConnection();

		if (Connect()) {
			std::cout << "Reconnected successfully" << std::endl;
			return;
		}
	}
}

// ==================== Utility Functions ====================

std::string NTRIPClient::BuildHTTPRequest(const std::string& path) {
	std::stringstream request;

	request << "GET " << path << " HTTP/1.1\r\n";
	request << "Host: " << config_.host << ":" << config_.port << "\r\n";
	request << "User-Agent: " << config_.user_agent << "\r\n";
	request << "Accept: */*\r\n";
	request << "Connection: keep-alive\r\n";
	request << "Ntrip-Version: Ntrip/2.0\r\n";

	// Authentication
	if (!config_.username.empty() && !config_.password.empty()) {
		std::string credentials = config_.username + ":" + config_.password;
		request << "Authorization: Basic " << Base64Encode(credentials) << "\r\n";
	}

	// NMEA GGA
	if (!config_.nmea_gga.empty()) {
		request << "Ntrip-GGA: " << config_.nmea_gga << "\r\n";
	}

	request << "\r\n";
	return request.str();
}

std::string NTRIPClient::Base64Encode(const std::string& input) {
	BIO *bio, *b64;
	BUF_MEM* buffer_ptr;

	b64 = BIO_new(BIO_f_base64());
	bio = BIO_new(BIO_s_mem());
	bio = BIO_push(b64, bio);

	BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
	BIO_write(bio, input.c_str(), input.length());
	BIO_flush(bio);
	BIO_get_mem_ptr(bio, &buffer_ptr);

	std::string result(buffer_ptr->data, buffer_ptr->length);
	BIO_free_all(bio);

	return result;
}
