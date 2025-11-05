#include "ntrip_client.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

#include "tool.h"



std::once_flag NTRIPClient::SSLManager::openssl_init_flag_;


NTRIPClient::NTRIPClient(const Config& config) :
	config_(config), socket_(std::make_unique<SocketManager>()), parser_(std::make_unique<RTCMParser>()) {
	if (config.use_ssl) {
		ssl_ = std::make_unique<SSLManager>();
	}
}


NTRIPClient::~NTRIPClient() {
	Disconnect();
}


bool NTRIPClient::Connect() {
	std::lock_guard<std::mutex> lock(config_mutex_);
	if (connected_) {
		return true;
	}
	try {
		// Report status
		ReportStatus("Connecting to " + config_.host + ":" + std::to_string(config_.port));
		// Create and configure socket
		if (!socket_->Create() || !socket_->Configure(config_)) {
			last_error_ = "Failed to create socket";
			ReportError(last_error_);
			return false;
		}
		// Connect socket
		if (!socket_->Connect(config_.host, config_.port, config_.connection_timeout)) {
			last_error_ = "Failed to connect to server";
			ReportError(last_error_);
			socket_->Close();
			return false;
		}
		// Initialize SSL if needed
		if (config_.use_ssl) {
			if (!ssl_->Initialize(socket_->GetFD(), config_.verify_ssl)) {
				last_error_ = "Failed to initialize SSL";
				ReportError(last_error_);
				socket_->Close();
				return false;
			}
			if (!ssl_->Connect()) {
				last_error_ = "SSL handshake failed";
				ReportError(last_error_);
				ssl_->Shutdown();
				socket_->Close();
				return false;
			}
		}
		// Send NTRIP request
		if (!SendHTTPRequest()) {
			last_error_ = "Failed to send NTRIP request";
			ReportError(last_error_);
			if (config_.use_ssl) {
				ssl_->Shutdown();
			}
			socket_->Close();
			return false;
		}
		// Receive and validate response
		if (!ReceiveHTTPResponse()) {
			ReportError(last_error_);
			if (config_.use_ssl) {
				ssl_->Shutdown();
			}
			socket_->Close();
			return false;
		}
		// Connection successful
		connected_ = true;
		stats_.connect_time = std::chrono::steady_clock::now();
		stats_.bytes_received = 0;
		stats_.messages_received = 0;
		ReportStatus("Connected to NTRIP caster: " + config_.host + "/" + config_.mountpoint);
		return true;
	} catch (const std::exception& e) {
		last_error_ = "Connection error: " + std::string(e.what());
		ReportError(last_error_);
		return false;
	}
}

void NTRIPClient::Disconnect() {
	StopReceiving();

	connected_ = false;

	if (config_.use_ssl && ssl_) {
		ssl_->Shutdown();
	}

	if (socket_) {
		socket_->Close();
	}

	ReportStatus("Disconnected");
}

void NTRIPClient::StartReceiving() {
	if (!connected_ || receiving_) {
		return;
	}

	receiving_ = true;
	receive_thread_ = std::make_unique<std::thread>(&NTRIPClient::ReceiveThread, this);
	process_thread_ = std::make_unique<std::thread>(&NTRIPClient::ProcessThread, this);

	ReportStatus("Started receiving RTCM data");
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

	// Clear queue
	std::lock_guard<std::mutex> lock(queue_mutex_);
	while (!data_queue_.empty()) {
		data_queue_.pop();
	}

	ReportStatus("Stopped receiving");
}

std::vector<NTRIPClient::MountPoint> NTRIPClient::GetSourceTable() {
	std::vector<MountPoint> mountpoints;

	// Create temporary connection
	SocketManager temp_socket;
	if (!temp_socket.Create() || !temp_socket.Configure(config_)) {
		return mountpoints;
	}

	if (!temp_socket.Connect(config_.host, config_.port, config_.connection_timeout)) {
		return mountpoints;
	}

	// Handle SSL if needed
	std::unique_ptr<SSLManager> temp_ssl;
	if (config_.use_ssl) {
		temp_ssl = std::make_unique<SSLManager>();
		if (!temp_ssl->Initialize(temp_socket.GetFD(), config_.verify_ssl) || !temp_ssl->Connect()) {
			return mountpoints;
		}
	}

	// Send request for source table
	std::string request = HTTPHandler::BuildRequest(config_, "GET", "/");
	ssize_t sent = config_.use_ssl ? temp_ssl->Send(request.c_str(), request.size()) : temp_socket.Send(request.c_str(), request.size());

	if (sent <= 0) {
		return mountpoints;
	}

	// Receive response
	std::string response;
	char buffer[4096];
	while (true) {
		ssize_t received = config_.use_ssl ? temp_ssl->Receive(buffer, sizeof(buffer) - 1) : temp_socket.Receive(buffer, sizeof(buffer) - 1);

		if (received <= 0)
			break;

		buffer[received] = '\0';
		response += buffer;

		// Check for complete response
		if (response.find("\r\n\r\n") != std::string::npos) {
			break;
		}
	}

	// Parse source table
	std::istringstream stream(response);
	std::string line;
	while (std::getline(stream, line)) {
		if (line.find("STR;") == 0) {
			MountPoint mp = ParseMountPoint(line);
			if (!mp.mountpoint.empty()) {
				mountpoints.push_back(mp);
			}
		}
	}

	return mountpoints;
}

void NTRIPClient::SetConfig(const Config& config) {
	std::lock_guard<std::mutex> lock(config_mutex_);
	config_ = config;
}

NTRIPClient::Config NTRIPClient::GetConfig() const {
	std::lock_guard<std::mutex> lock(config_mutex_);
	return config_;
}

void NTRIPClient::UpdateNMEA(const std::string& gga) {
	std::lock_guard<std::mutex> lock(config_mutex_);
	config_.nmea_gga = gga;
}

// ==================== Private Methods ====================

bool NTRIPClient::SendHTTPRequest() {
	std::string request = HTTPHandler::BuildRequest(config_, "GET", "/" + config_.mountpoint);
	size_t total_sent = 0;

	while (total_sent < request.size()) {
		ssize_t sent = SendData(request.c_str() + total_sent, request.size() - total_sent);
		if (sent <= 0) {
			return false;
		}
		total_sent += sent;
	}

	return true;
}

bool NTRIPClient::ReceiveHTTPResponse() {
	std::string buffer;
	char temp[1024];

	// Read until we have complete headers
	while (buffer.find("\r\n\r\n") == std::string::npos) {
		ssize_t received = ReceiveData(temp, sizeof(temp) - 1);
		if (received <= 0) {
			last_error_ = "Failed to receive HTTP response";
			return false;
		}
		temp[received] = '\0';
		buffer += temp;
	}

	// Parse response
	HTTPHandler::Response response = HTTPHandler::ParseResponse(buffer);

	if (response.status_code != 200) {
		switch (response.status_code) {
			case 401:
				last_error_ = "Authentication failed (401)";
				break;
			case 404:
				last_error_ = "Mount point not found: " + config_.mountpoint + " (404)";
				break;
			case 403:
				last_error_ = "Access forbidden (403)";
				break;
			default:
				last_error_ = "HTTP error: " + std::to_string(response.status_code);
		}
		return false;
	}

	return true;
}

void NTRIPClient::ReceiveThread() {
	uint8_t buffer[4096];

	while (receiving_ && connected_) {
		ssize_t received = ReceiveData(buffer, sizeof(buffer));

		if (received > 0) {
			// Update statistics
			stats_.bytes_received += received;

			// Add to queue for processing
			DataPacket packet;
			packet.data.assign(buffer, buffer + received);
			packet.timestamp = std::chrono::steady_clock::now();

			{
				std::lock_guard<std::mutex> lock(queue_mutex_);
				if (data_queue_.size() < MAX_QUEUE_SIZE) {
					data_queue_.push(std::move(packet));
					queue_cv_.notify_one();
				}
			}
		} else if (received == 0) {
			// Connection closed
			last_error_ = "Connection closed by server";
			connected_ = false;

			if (config_.auto_reconnect && receiving_) {
				HandleReconnection();
			}
		} else {
			// Error occurred
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT) {
				last_error_ = "Receive error: " + std::string(strerror(errno));
				connected_ = false;

				if (config_.auto_reconnect && receiving_) {
					HandleReconnection();
				}
			}
		}
	}
}

void NTRIPClient::ProcessThread() {
	while (receiving_) {
		std::unique_lock<std::mutex> lock(queue_mutex_);

		// Wait for data
		queue_cv_.wait(lock, [this] { return !data_queue_.empty() || !receiving_; });

		while (!data_queue_.empty() && receiving_) {
			DataPacket packet = std::move(data_queue_.front());
			data_queue_.pop();
			lock.unlock();

			// Parse RTCM messages
			auto messages = parser_->Parse(packet.data.data(), packet.data.size());

			for (const auto& msg: messages) {
				stats_.messages_received++;

				// Deliver via callback
				if (data_callback_) {
					data_callback_(msg.data.data(), msg.data.size());
				}
			}

			lock.lock();
		}
	}
}

void NTRIPClient::HandleReconnection() {
	int attempts = 0;

	while (config_.auto_reconnect && receiving_) {
		std::this_thread::sleep_for(std::chrono::seconds(config_.reconnect_interval));

		attempts++;
		stats_.reconnect_count++;

		ReportStatus("Reconnection attempt #" + std::to_string(attempts));

		// Close existing connection
		if (config_.use_ssl && ssl_) {
			ssl_->Shutdown();
		}
		socket_->Close();

		// Try to reconnect
		if (Connect()) {
			ReportStatus("Reconnected successfully");
			reconnect_attempts_ = 0;
			return;
		}

		ReportError("Reconnection failed: " + last_error_);
	}
}

ssize_t NTRIPClient::SendData(const void* data, size_t size) {
	if (config_.use_ssl && ssl_ && ssl_->IsValid()) {
		return ssl_->Send(data, size);
	}
	return socket_->Send(data, size);
}

ssize_t NTRIPClient::ReceiveData(void* buffer, size_t size) {
	if (config_.use_ssl && ssl_ && ssl_->IsValid()) {
		return ssl_->Receive(buffer, size);
	}
	return socket_->Receive(buffer, size);
}

void NTRIPClient::ReportError(const std::string& error) {
	if (error_callback_) {
		error_callback_(error);
	}
}

void NTRIPClient::ReportStatus(const std::string& status) {
	if (status_callback_) {
		status_callback_(status);
	}
}

// ==================== Socket Manager ====================

NTRIPClient::SocketManager::SocketManager() : fd_(-1) {}

NTRIPClient::SocketManager::~SocketManager() {
	Close();
}

bool NTRIPClient::SocketManager::Create() {
	fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
	if (fd_ < 0) {
		return false;
	}
	return true;
}

bool NTRIPClient::SocketManager::Configure(const Config& config) {
	return SetSocketOptions(config) && SetTimeouts(config.connection_timeout);
}

bool NTRIPClient::SocketManager::Connect(const std::string& host, int port, int timeout_sec) {
	struct hostent* host_entry = gethostbyname(host.c_str());
	if (!host_entry) {
		return false;
	}

	struct sockaddr_in server_addr{};
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	memcpy(&server_addr.sin_addr.s_addr, host_entry->h_addr_list[0], host_entry->h_length);

	if (::connect(fd_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
		return false;
	}

	return true;
}

void NTRIPClient::SocketManager::Close() {
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
}

ssize_t NTRIPClient::SocketManager::Send(const void* data, size_t size) {
	if (fd_ < 0)
		return -1;
	return ::send(fd_, data, size, MSG_NOSIGNAL);
}

ssize_t NTRIPClient::SocketManager::Receive(void* buffer, size_t size) {
	if (fd_ < 0)
		return -1;
	return ::recv(fd_, buffer, size, 0);
}

bool NTRIPClient::SocketManager::SetSocketOptions(const Config& config) {
	// Enable address reuse
	int reuse = 1;
	setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	// Disable Nagle's algorithm
	int nodelay = 1;
	setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

	// Set buffer sizes
	if (config.receive_buffer_size > 0) {
		setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &config.receive_buffer_size, sizeof(config.receive_buffer_size));
	}

	// Set non-blocking if requested
	if (config.use_nonblocking) {
		return SetNonBlocking(true);
	}

	return true;
}

bool NTRIPClient::SocketManager::SetTimeouts(int timeout_sec) {
	struct timeval tv;
	tv.tv_sec = timeout_sec;
	tv.tv_usec = 0;

	if (setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		return false;
	}

	if (setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
		return false;
	}

	return true;
}

bool NTRIPClient::SocketManager::SetNonBlocking(bool enable) {
	int flags = fcntl(fd_, F_GETFL, 0);
	if (flags < 0)
		return false;

	if (enable) {
		flags |= O_NONBLOCK;
	} else {
		flags &= ~O_NONBLOCK;
	}

	return fcntl(fd_, F_SETFL, flags) >= 0;
}

// ==================== SSL Manager ====================

NTRIPClient::SSLManager::SSLManager() : ctx_(nullptr), ssl_(nullptr) {
	std::call_once(openssl_init_flag_, InitializeOpenSSL);
}

NTRIPClient::SSLManager::~SSLManager() {
	Shutdown();
}

void NTRIPClient::SSLManager::InitializeOpenSSL() {
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
}

bool NTRIPClient::SSLManager::Initialize(int socket_fd, bool verify_ssl) {
	const SSL_METHOD* method = TLS_client_method();
	ctx_ = SSL_CTX_new(method);
	if (!ctx_) {
		return false;
	}

	if (!verify_ssl) {
		SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
	} else {
		SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
		SSL_CTX_set_default_verify_paths(ctx_);
	}

	ssl_ = SSL_new(ctx_);
	if (!ssl_) {
		SSL_CTX_free(ctx_);
		ctx_ = nullptr;
		return false;
	}

	SSL_set_fd(ssl_, socket_fd);
	return true;
}

bool NTRIPClient::SSLManager::Connect() {
	int ret = SSL_connect(ssl_);
	return ret > 0;
}

void NTRIPClient::SSLManager::Shutdown() {
	if (ssl_) {
		SSL_shutdown(ssl_);
		SSL_free(ssl_);
		ssl_ = nullptr;
	}
	if (ctx_) {
		SSL_CTX_free(ctx_);
		ctx_ = nullptr;
	}
}

ssize_t NTRIPClient::SSLManager::Send(const void* data, size_t size) {
	if (!ssl_)
		return -1;
	return SSL_write(ssl_, data, size);
}

ssize_t NTRIPClient::SSLManager::Receive(void* buffer, size_t size) {
	if (!ssl_)
		return -1;
	return SSL_read(ssl_, buffer, size);
}

// ==================== HTTP Handler ====================

std::string NTRIPClient::HTTPHandler::BuildRequest(const Config& config, const std::string& method, const std::string& path) {
	std::stringstream request;

	// Request line
	request << method << " " << path << " HTTP/1.1\r\n";

	// Headers
	request << "Host: " << config.host << ":" << config.port << "\r\n";
	request << "User-Agent: " << config.user_agent << "\r\n";
	request << "Accept: */*\r\n";
	request << "Connection: keep-alive\r\n";
	request << "Ntrip-Version: Ntrip/2.0\r\n";

	// Authentication
	if (!config.username.empty() && !config.password.empty()) {
		request << "Authorization: Basic " << BuildAuthHeader(config) << "\r\n";
	}

	// NMEA GGA
	if (!config.nmea_gga.empty()) {
		request << "Ntrip-GGA: " << config.nmea_gga << "\r\n";
	}

	request << "\r\n";
	return request.str();
}


NTRIPClient::HTTPHandler::Response NTRIPClient::HTTPHandler::ParseResponse(const std::string& data) {
	Response response;
	std::istringstream stream(data);
	std::string line;

	// Parse status line
	if (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		std::istringstream status_stream(line);
		std::string http_version;
		status_stream >> http_version >> response.status_code;
		std::getline(status_stream, response.status_text);
	}

	// Parse headers
	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		if (line.empty()) {
			break;
		}

		size_t colon_pos = line.find(':');
		if (colon_pos != std::string::npos) {
			std::string key = line.substr(0, colon_pos);
			std::string value = line.substr(colon_pos + 1);

			// Trim whitespace
			key.erase(0, key.find_first_not_of(" \t"));
			key.erase(key.find_last_not_of(" \t") + 1);
			value.erase(0, value.find_first_not_of(" \t"));
			value.erase(value.find_last_not_of(" \t") + 1);

			response.headers[key] = value;
		}
	}

	// Remaining is body
	std::string remaining;
	while (std::getline(stream, line)) {
		remaining += line + "\n";
	}
	response.body = remaining;

	return response;
}


std::string NTRIPClient::HTTPHandler::BuildAuthHeader(const Config& config) {
	std::string credentials = config.username + ":" + config.password;
	return EncodeBase64(credentials);
}


std::string NTRIPClient::HTTPHandler::EncodeBase64(const std::string& input) {
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

// ==================== RTCM Parser ====================

NTRIPClient::RTCMParser::RTCMParser() {
	buffer_.reserve(8192);
}


void NTRIPClient::RTCMParser::Reset() {
	buffer_.clear();
}


std::vector<NTRIPClient::RTCMParser::Message> NTRIPClient::RTCMParser::Parse(const uint8_t* data, size_t size) {
	std::vector<Message> messages;

	// Add new data to buffer
	buffer_.insert(buffer_.end(), data, data + size);

	// Parse frames
	size_t offset = 0;
	while (offset < buffer_.size()) {
		std::vector<uint8_t> frame;
		if (FindFrame(offset, frame)) {
			Message msg;
			msg.type = ExtractMessageType(frame);
			msg.data = std::move(frame);
			messages.push_back(std::move(msg));
		} else {
			break;
		}
	}

	// Remove processed data
	if (offset > 0) {
		buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
	}

	// Prevent buffer overflow
	if (buffer_.size() > 16384) {
		buffer_.clear();
	}

	return messages;
}


bool NTRIPClient::RTCMParser::FindFrame(size_t& offset, std::vector<uint8_t>& frame) {
	// Find preamble
	while (offset < buffer_.size() && buffer_[offset] != PREAMBLE) {
		offset++;
	}

	if (offset >= buffer_.size()) {
		return false;
	}

	// Need at least 3 bytes for header
	if (offset + 3 > buffer_.size()) {
		return false;
	}

	// Extract message length
	uint16_t length = ((buffer_[offset + 1] & 0x03) << 8) | buffer_[offset + 2];

	if (length > MAX_MESSAGE_SIZE) {
		offset++;
		return false;
	}

	// Check for complete frame
	size_t frame_size = 3 + length + 3;
	if (offset + frame_size > buffer_.size()) {
		return false;
	}

	// Extract frame
	frame.assign(buffer_.begin() + offset, buffer_.begin() + offset + frame_size);

	// Validate CRC
	if (!ValidateCRC(frame)) {
		offset++;
		return false;
	}

	offset += frame_size;
	return true;
}


bool NTRIPClient::RTCMParser::ValidateCRC(const std::vector<uint8_t>& frame) {
	size_t length = frame.size() - 3;
	uint32_t computed = Tool::CRC::CalculateRTCM3_CRC24(frame.data(), length);
	uint32_t received = (frame[length] << 16) | (frame[length + 1] << 8) | frame[length + 2];
	return computed == received;
}


uint16_t NTRIPClient::RTCMParser::ExtractMessageType(const std::vector<uint8_t>& frame) {
	if (frame.size() < 5)
		return 0;
	return ((frame[3] << 4) | (frame[4] >> 4)) & 0xFFF;
}

// ==================== Statistics ====================

double NTRIPClient::Statistics::GetDataRate() const {
	auto uptime = GetUptime();
	if (uptime.count() == 0)
		return 0.0;
	return static_cast<double>(bytes_received) / uptime.count() / 1024.0;
}

std::chrono::seconds NTRIPClient::Statistics::GetUptime() const {
	if (connect_time == std::chrono::steady_clock::time_point{}) {
		return std::chrono::seconds{ 0 };
	}
	auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::seconds>(now - connect_time);
}

void NTRIPClient::Statistics::Reset() {
	connect_time = std::chrono::steady_clock::time_point{};
	bytes_received = 0;
	messages_received = 0;
	crc_errors = 0;
	reconnect_count = 0;
}

// ==================== Utility Functions ====================

std::string NTRIPClient::Trim(const std::string& str) {
	size_t first = str.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return "";
	size_t last = str.find_last_not_of(" \t\r\n");
	return str.substr(first, (last - first + 1));
}

std::vector<std::string> NTRIPClient::Split(const std::string& str, char delimiter) {
	std::vector<std::string> tokens;
	std::stringstream ss(str);
	std::string token;
	while (std::getline(ss, token, delimiter)) {
		tokens.push_back(token);
	}
	return tokens;
}

NTRIPClient::MountPoint NTRIPClient::ParseMountPoint(const std::string& line) {
	MountPoint mp;
	std::vector<std::string> fields = Split(line, ';');

	if (fields.size() > 1)
		mp.mountpoint = fields[1];
	if (fields.size() > 2)
		mp.city = fields[2];
	if (fields.size() > 3)
		mp.data_format = fields[3];
	if (fields.size() > 4)
		mp.format_details = fields[4];
	if (fields.size() > 5 && !fields[5].empty()) {
		try {
			mp.carrier = std::stoi(fields[5]);
		} catch (...) {
		}
	}
	if (fields.size() > 6)
		mp.nav_system = fields[6];
	if (fields.size() > 7)
		mp.network = fields[7];
	if (fields.size() > 8)
		mp.country = fields[8];
	if (fields.size() > 9 && !fields[9].empty()) {
		try {
			mp.latitude = std::stod(fields[9]);
		} catch (...) {
		}
	}
	if (fields.size() > 10 && !fields[10].empty()) {
		try {
			mp.longitude = std::stod(fields[10]);
		} catch (...) {
		}
	}

	return mp;
}
