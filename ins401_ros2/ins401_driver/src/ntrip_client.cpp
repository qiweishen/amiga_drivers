#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <map>
#include <sys/socket.h>
#include <openssl/bio.h>

#include "ntrip_client.h"
#include "tool.h"



// Initialize static member
std::once_flag NTRIPClient::openssl_init_flag_;


NTRIPClient::NTRIPClient(std::string host, const int port, const bool is_ssl,
                         std::string username, std::string password,
                         std::string mountpoint)
	: host_(std::move(host)), port_(port), is_ssl_(is_ssl),
	  username_(std::move(username)), password_(std::move(password)),
	  mountpoint_(std::move(mountpoint)),
	  socket_fd_(-1), ssl_ctx_(nullptr), ssl_(nullptr),
	  socket_initialized_(false) {
	// Initialize OpenSSL once for all instances
	std::call_once(openssl_init_flag_, InitializeOpenSSL);
}


NTRIPClient::~NTRIPClient() {
	Disconnect();
}


void NTRIPClient::InitializeOpenSSL() {
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
}


void NTRIPClient::CleanupOpenSSL() {
	EVP_cleanup();
	ERR_free_strings();
}


bool NTRIPClient::Connect() {
	if (connected_.load()) {
		return true;
	}
	try {
		// Initialize and connect socket
		if (!InitializeSocket()) {
			last_error_ = "Failed to initialize socket";
			return false;
		}
		if (!ConnectSocket()) {
			last_error_ = "Failed to connect socket: " + last_error_;
			CloseSocket();
			return false;
		}
		// Initialize SSL for HTTPS connections
		if (is_ssl_) {
			if (!InitializeSSL()) {
				last_error_ = "Failed to initialize SSL";
				CloseSocket();
				return false;
			}
			if (!ConnectSSL()) {
				last_error_ = "SSL handshake failed";
				CleanupSSL();
				CloseSocket();
				return false;
			}
		}

		// Send HTTP request for the mountpoint
		if (!SendHTTPRequest("GET", "/" + mountpoint_)) {
			last_error_ = "Failed to send HTTP request";
			if (is_ssl_) CleanupSSL();
			CloseSocket();
			return false;
		}
		// Receive and parse HTTP response
		HTTPResponse response;
		if (!ReceiveHTTPResponse(response)) {
			last_error_ = "Failed to receive HTTP response";
			if (is_ssl_) CleanupSSL();
			CloseSocket();
			return false;
		}
		// Check response status
		if (response.status_code != 200) {
			switch (response.status_code) {
				case 401:
					last_error_ = "Authentication failed (401)";
					break;
				case 404:
					last_error_ = "Mount point not found: " + mountpoint_ + " (404)";
					break;
				case 403:
					last_error_ = "Access forbidden (403)";
					break;
				case 503:
					last_error_ = "Service unavailable (503)";
					break;
				default:
					last_error_ = "HTTP error: " + std::to_string(response.status_code);
			}
			if (is_ssl_) CleanupSSL();
			CloseSocket();
			return false;
		}

		connected_ = true;
		start_time_ = std::chrono::steady_clock::now();
		bytes_received_ = 0;
		messages_received_ = 0;
		std::cout << "Connected to NTRIP caster: " << host_ << ":" << port_
				<< "/" << mountpoint_ << std::endl;
		return true;
	} catch (const std::exception &e) {
		last_error_ = "Connection error: " + std::string(e.what());
		if (is_ssl_) CleanupSSL();
		CloseSocket();
		return false;
	}
}


bool NTRIPClient::InitializeSocket() {
	// Create TCP socket
	socket_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_fd_ < 0) {
		last_error_ = "Failed to create socket: " + std::string(strerror(errno));
		return false;
	}
	socket_initialized_ = true;
	// Set socket timeout
	if (!SetSocketTimeout(connection_timeout_)) {
		return false;
	}
	return true;
}


bool NTRIPClient::ConnectSocket() {
	// Resolve hostname to IP address
	hostent *host_entry = gethostbyname(host_.c_str());
	if (host_entry == nullptr) {
		last_error_ = "Failed to resolve hostname: " + host_;
		return false;
	}
	// Setup socket address structure
	sockaddr_in server_addr{};
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port_);
	memcpy(&server_addr.sin_addr.s_addr, host_entry->h_addr_list[0], host_entry->h_length);
	// Connect to server
	if (connect(socket_fd_, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0) {
		last_error_ = "Connection failed: " + std::string(strerror(errno));
		return false;
	}
	return true;
}


void NTRIPClient::CloseSocket() {
	if (socket_fd_ >= 0) {
		close(socket_fd_);
		socket_fd_ = -1;
	}
}


bool NTRIPClient::SetSocketTimeout(int timeout_sec) {
	timeval tv{};
	tv.tv_sec = timeout_sec;
	tv.tv_usec = 0;
	// Set receive timeout
	if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		last_error_ = "Failed to set receive timeout";
		return false;
	}
	// Set send timeout
	if (setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, (const char *) &tv, sizeof(tv)) < 0) {
		last_error_ = "Failed to set send timeout";
		return false;
	}
	return true;
}


bool NTRIPClient::InitializeSSL() {
	// Create SSL context
	const SSL_METHOD *method = TLS_client_method();
	ssl_ctx_ = SSL_CTX_new(method);
	if (!ssl_ctx_) {
		last_error_ = "Failed to create SSL context";
		return false;
	}

	// Configure SSL verification
	if (!verify_ssl_) {
		SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
	} else {
		SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
		// Load default certificate locations
		SSL_CTX_set_default_verify_paths(ssl_ctx_);
	}

	// Create SSL connection
	ssl_ = SSL_new(ssl_ctx_);
	if (!ssl_) {
		last_error_ = "Failed to create SSL connection";
		SSL_CTX_free(ssl_ctx_);
		ssl_ctx_ = nullptr;
		return false;
	}

	// Attach socket to SSL
	SSL_set_fd(ssl_, socket_fd_);

	return true;
}


bool NTRIPClient::ConnectSSL() {
	// Perform SSL handshake
	int ret = SSL_connect(ssl_);
	if (ret <= 0) {
		int ssl_error = SSL_get_error(ssl_, ret);
		last_error_ = "SSL handshake failed with error code: " + std::to_string(ssl_error);
		return false;
	}
	// Optionally verify certificate
	if (verify_ssl_) {
		X509 *cert = SSL_get_peer_certificate(ssl_);
		if (cert) {
			X509_free(cert);
		} else {
			last_error_ = "No certificate provided by server";
			return false;
		}
		if (SSL_get_verify_result(ssl_) != X509_V_OK) {
			last_error_ = "Certificate verification failed";
			return false;
		}
	}
	return true;
}


void NTRIPClient::CleanupSSL() {
	if (ssl_) {
		SSL_shutdown(ssl_);
		SSL_free(ssl_);
		ssl_ = nullptr;
	}
	if (ssl_ctx_) {
		SSL_CTX_free(ssl_ctx_);
		ssl_ctx_ = nullptr;
	}
}


ssize_t NTRIPClient::SendData(const void *data, size_t size) const {
	if (is_ssl_ && ssl_) {
		return SSL_write(ssl_, data, size);
	}
	if (socket_fd_ >= 0) {
		return send(socket_fd_, data, size, 0);
	}
	return -1;
}


ssize_t NTRIPClient::ReceiveData(void *buffer, const size_t size) const {
	if (is_ssl_ && ssl_) {
		return SSL_read(ssl_, buffer, size);
	}
	if (socket_fd_ >= 0) {
		return recv(socket_fd_, buffer, size, 0);
	}
	return -1;
}


std::string NTRIPClient::BuildHTTPRequest(const std::string &method, const std::string &path) const {
	std::stringstream request;
	// Request line
	request << method << " " << path << " HTTP/1.1\r\n";
	// Headers
	request << "Host: " << host_ << ":" << port_ << "\r\n";
	request << "User-Agent: " << user_agent_ << "\r\n";
	request << "Accept: */*\r\n";
	request << "Connection: keep-alive\r\n";
	request << "Ntrip-Version: Ntrip/2.0\r\n";
	// Authentication header if credentials provided
	if (!username_.empty() && !password_.empty()) {
		request << "Authorization: Basic " << BuildAuthHeader() << "\r\n";
	}
	// NMEA GGA for VRS (Virtual Reference Station)
	if (!nmea_gga_.empty()) {
		request << "Ntrip-GGA: " << nmea_gga_ << "\r\n";
	}
	// End of headers
	request << "\r\n";
	return request.str();
}


std::string NTRIPClient::BuildAuthHeader() const {
	std::string credentials = username_ + ":" + password_;
	return Base64Encode(credentials);
}


std::string NTRIPClient::Base64Encode(const std::string &input) {
	static const std::string base64_chars =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz"
			"0123456789+/";

	std::string encoded;
	int i = 0;
	int j = 0;
	uint8_t char_array_3[3];
	uint8_t char_array_4[4];

	for (const char idx: input) {
		char_array_3[i++] = idx;
		if (i == 3) {
			char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
			char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
			char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
			char_array_4[3] = char_array_3[2] & 0x3f;

			for (i = 0; i < 4; i++)
				encoded += base64_chars[char_array_4[i]];
			i = 0;
		}
	}

	if (i) {
		for (j = i; j < 3; j++)
			char_array_3[j] = '\0';

		char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
		char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
		char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

		for (j = 0; j < i + 1; j++)
			encoded += base64_chars[char_array_4[j]];

		while (i++ < 3)
			encoded += '=';
	}

	return encoded;
}


bool NTRIPClient::SendHTTPRequest(const std::string &method, const std::string &path) {
	const std::string request = BuildHTTPRequest(method, path);
	size_t total_sent = 0;
	while (total_sent < request.size()) {
		ssize_t sent = SendData(request.c_str() + total_sent, request.size() - total_sent);
		if (sent <= 0) {
			last_error_ = "Failed to send HTTP request";
			return false;
		}
		total_sent += sent;
	}
	return true;
}


bool NTRIPClient::ReceiveHTTPResponse(HTTPResponse &response) {
	std::string buffer;
	char temp[1024];
	bool headers_complete = false;
	// Read headers
	while (!headers_complete) {
		ssize_t received = ReceiveData(temp, sizeof(temp) - 1);
		if (received <= 0) {
			last_error_ = "Failed to receive HTTP response";
			return false;
		}
		temp[received] = '\0';
		buffer += temp;
		// Check for end of headers
		size_t header_end = buffer.find("\r\n\r\n");
		if (header_end != std::string::npos) {
			headers_complete = true;
			// Parse headers
			std::string headers_str = buffer.substr(0, header_end);
			if (!ParseHTTPResponse(headers_str, response)) {
				return false;
			}
			// Save any body data received with headers
			if (header_end + 4 < buffer.size()) {
				response.body = buffer.substr(header_end + 4);
			}
		}
	}
	return true;
}


bool NTRIPClient::ParseHTTPResponse(const std::string &data, HTTPResponse &response) {
	std::istringstream stream(data);
	std::string line;
	// Parse status line
	if (!std::getline(stream, line)) {
		last_error_ = "Invalid HTTP response";
		return false;
	}
	// Remove \r if present
	if (!line.empty() && line.back() == '\r') {
		line.pop_back();
	}
	// Parse status line: HTTP/1.1 200 OK
	std::istringstream status_stream(line);
	std::string http_version;
	status_stream >> http_version >> response.status_code;
	std::getline(status_stream, response.status_text);
	response.status_text = Trim(response.status_text);
	// Parse headers
	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty()) {
			break;
		}
		if (size_t colon_pos = line.find(':'); colon_pos != std::string::npos) {
			std::string key = Trim(line.substr(0, colon_pos));
			std::string value = Trim(line.substr(colon_pos + 1));
			// Convert to lowercase for case-insensitive comparison
			std::ranges::transform(key, key.begin(), ::tolower);
			response.headers[key] = value;
		}
	}
	// Check for chunked transfer encoding
	if (auto transfer_encoding = response.headers.find("transfer-encoding");
		transfer_encoding != response.headers.end()) {
		response.is_chunked = (transfer_encoding->second.find("chunked") != std::string::npos);
	}
	return true;
}


void NTRIPClient::Disconnect() {
	StopReceiving();
	connected_ = false;
	if (is_ssl_) {
		CleanupSSL();
	}
	CloseSocket();
}


void NTRIPClient::StartReceiving() {
	if (!connected_ || receiving_) {
		return;
	}
	receiving_ = true;
	receive_thread_ = std::make_unique<std::thread>(&NTRIPClient::ReceiveLoop, this);
	process_thread_ = std::make_unique<std::thread>(&NTRIPClient::ProcessLoop, this);
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
	// Clear data queue
	std::lock_guard<std::mutex> lock(queue_mutex_);
	while (!data_queue_.empty()) {
		data_queue_.pop();
	}
}


void NTRIPClient::ReceiveLoop() {
	std::vector<uint8_t> rtcm_buffer;
	rtcm_buffer.reserve(8192);
	uint8_t temp_buffer[4096];
	// Set socket to non-blocking mode for better control
	SetSocketTimeout(1); // 1 second timeout for checking stop condition
	while (receiving_ && connected_) {
		try {
			// Receive data from socket
			if (ssize_t received = ReceiveData(temp_buffer, sizeof(temp_buffer)); received > 0) {
				// Update statistics
				bytes_received_ += received;
				// Add to RTCM buffer
				rtcm_buffer.insert(rtcm_buffer.end(), temp_buffer, temp_buffer + received);
				// Parse RTCM frames
				size_t offset = 0;
				while (offset < rtcm_buffer.size()) {
					std::vector<uint8_t> message;
					if (ParseRTCMFrame(rtcm_buffer, offset, message)) {
						// Add to processing queue
						{
							std::lock_guard lock(queue_mutex_);
							data_queue_.push(std::move(message));
						}
						queue_cv_.notify_one();
						messages_received_++;
					} else {
						break;
					}
				}
				// Remove processed data from buffer
				if (offset > 0) {
					rtcm_buffer.erase(rtcm_buffer.begin(), rtcm_buffer.begin() + offset);
				}
				// Prevent buffer overflow
				if (rtcm_buffer.size() > 16384) {
					std::cerr << "RTCM buffer overflow, clearing" << std::endl;
					rtcm_buffer.clear();
				}
			} else if (received == 0) {
				// Connection closed by server
				last_error_ = "Connection closed by server";
				connected_ = false;

				if (auto_reconnect_ && receiving_) {
					HandleReconnection();
				}
			} else {
				// Check for timeout vs actual error
				if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT) {
					last_error_ = "Receive error: " + std::string(strerror(errno));
					connected_ = false;

					if (auto_reconnect_ && receiving_) {
						HandleReconnection();
					}
				}
			}
		} catch (const std::exception &e) {
			last_error_ = "Receive error: " + std::string(e.what());
			connected_ = false;

			if (auto_reconnect_ && receiving_) {
				HandleReconnection();
			}
		}
	}
}


void NTRIPClient::ProcessLoop() {
	while (receiving_) {
		std::unique_lock<std::mutex> lock(queue_mutex_);

		// Wait for data or stop signal
		queue_cv_.wait(lock, [this] {
			return !data_queue_.empty() || !receiving_;
		});
		// Process all available messages
		while (!data_queue_.empty() && receiving_) {
			auto message = std::move(data_queue_.front());
			data_queue_.pop();
			lock.unlock();
			// Deliver RTCM message via callback
			if (rtcm_callback_) {
				rtcm_callback_(message.data(), message.size());
			}
			lock.lock();
		}
	}
}


bool NTRIPClient::ParseRTCMFrame(const std::vector<uint8_t> &buffer, size_t &offset,
                                 std::vector<uint8_t> &message) {
	const size_t buff_size = buffer.size();
	// Find RTCM3 preamble byte (0xD3)
	while (offset < buff_size && buffer[offset] != RTCM3PREAMB) {
		offset++;
	}
	if (offset >= buff_size) {
		return false;
	}
	// Need 3 bytes for (preamble + 2 length bytes)
	if (offset + 3 > buffer.size()) {
		return false;
	}
	// Extract message length (10 bits)
	uint16_t length = ((buffer[offset + 1] & 0x03) << 8) | buffer[offset + 2];
	// Validate length (RTCM3 maximum is 1023 bytes)
	if (length > 1023) {
		offset++; // Skip invalid sync byte
		return false;
	}
	// Check for complete frame (header + message + CRC)
	size_t frame_size = 3 + length + 3;
	if (offset + frame_size > buffer.size()) {
		return false; // Wait for more data
	}
	// Validate CRC-24
	uint32_t computed_crc = Tool::CRC::CalculateRTCM3_CRC24(&buffer[offset], 3 + length);
	uint32_t received_crc = (buffer[offset + 3 + length] << 16) |
	                        (buffer[offset + 3 + length + 1] << 8) |
	                        (buffer[offset + 3 + length + 2]);
	if (computed_crc != received_crc) {
		std::cerr << "CRC error in RTCM message" << std::endl;
		offset++; // Skip invalid sync byte
		return false;
	}
	// Extract complete message
	message.assign(buffer.begin() + offset, buffer.begin() + offset + frame_size);
	offset += frame_size;
	return true;
}


void NTRIPClient::HandleReconnection() {
	int attempt = 0;

	while (auto_reconnect_ && receiving_) {
		std::this_thread::sleep_for(std::chrono::seconds(reconnect_interval_));

		std::cout << "Reconnection attempt #" << ++attempt << "..." << std::endl;

		// Close existing connection
		if (is_ssl_) {
			CleanupSSL();
		}
		CloseSocket();

		// Try to reconnect
		if (Connect()) {
			std::cout << "Reconnected successfully!" << std::endl;
			return;
		}

		std::cerr << "Reconnection failed: " << last_error_ << std::endl;
	}
}


std::vector<MountPoint> NTRIPClient::GetSourceTable() {
	std::vector<MountPoint> mountpoints;
	try {
		// Create temporary connection for source table
		if (!InitializeSocket() || !ConnectSocket()) {
			return mountpoints;
		}
		// Initialize SSL if needed
		if (is_ssl_) {
			if (!InitializeSSL() || !ConnectSSL()) {
				CloseSocket();
				return mountpoints;
			}
		}
		// Request source table
		if (!SendHTTPRequest("GET", "/")) {
			if (is_ssl_) CleanupSSL();
			CloseSocket();
			return mountpoints;
		}
		// Receive response
		HTTPResponse response;
		if (!ReceiveHTTPResponse(response)) {
			if (is_ssl_) CleanupSSL();
			CloseSocket();
			return mountpoints;
		}
		// Read body content
		char buffer[4096];
		while (true) {
			const ssize_t received = ReceiveData(buffer, sizeof(buffer) - 1);
			if (received <= 0) {
				break;
			}
			buffer[received] = '\0';
			response.body += buffer;
		}
		// Clean up temporary connection
		if (is_ssl_) {
			CleanupSSL();
		}
		CloseSocket();
		// Parse source table
		std::istringstream stream(response.body);
		std::string line;
		while (std::getline(stream, line)) {
			if (line.find("STR;") == 0) {
				auto mp = ParseMountPointLine(line);
				if (!mp.mountpoint.empty()) {
					mountpoints.push_back(mp);
				}
			}
		}
	} catch (const std::exception &e) {
		last_error_ = "Failed to get source table: " + std::string(e.what());
	}
	return mountpoints;
}


MountPoint NTRIPClient::ParseMountPointLine(const std::string &line) {
	MountPoint mp;
	std::vector<std::string> fields = Split(line, ';');
	// Parse SOURCETABLE fields
	if (fields.size() > 1) mp.mountpoint = fields[1];
	if (fields.size() > 2) mp.city = fields[2];
	if (fields.size() > 3) mp.data_format = fields[3];
	if (fields.size() > 4) mp.format_details = fields[4];
	if (fields.size() > 5 && !fields[5].empty()) mp.carrier = std::stoi(fields[5]);
	if (fields.size() > 6) mp.nav_system = fields[6];
	if (fields.size() > 7) mp.network = fields[7];
	if (fields.size() > 8) mp.country = fields[8];
	if (fields.size() > 9 && !fields[9].empty()) mp.latitude = std::stod(fields[9]);
	if (fields.size() > 10 && !fields[10].empty()) mp.longitude = std::stod(fields[10]);
	return mp;
}


std::string NTRIPClient::Trim(const std::string &str) {
	size_t first = str.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	size_t last = str.find_last_not_of(" \t\r\n");
	return str.substr(first, (last - first + 1));
}


std::vector<std::string> NTRIPClient::Split(const std::string &str, char delimiter) {
	std::vector<std::string> tokens;
	std::stringstream ss(str);
	std::string token;

	while (std::getline(ss, token, delimiter)) {
		tokens.push_back(token);
	}

	return tokens;
}


double NTRIPClient::GetDataRate() const {
	if (!connected_ || bytes_received_ == 0) {
		return 0.0;
	}
	auto now = std::chrono::steady_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::seconds>(
		now - start_time_).count();
	if (duration == 0) {
		return 0.0;
	}
	return static_cast<double>(bytes_received_.load()) / duration / 1024.0; // KB/s
}
