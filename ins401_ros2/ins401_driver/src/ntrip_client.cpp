#include "ntrip_client.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <utility>

NTRIPClient::NTRIPClient(std::string &host, int port,
                         std::string &username,
                         std::string &password,
                         std::string &mountpoint)
	: socket_fd_(-1),
	  host_(host),
	  port_(port),
	  username_(std::move(username)),
	  password_(std::move(password)),
	  mountpoint_(std::move(mountpoint)),
	  user_agent_("NTRIP C++ Client/1.0"),
	  connected_(false),
	  receiving_(false),
	  bytes_received_(0),
	  messages_received_(0),
	  connection_timeout_(10),
	  receive_timeout_(30),
	  auto_reconnect_(true),
	  reconnect_interval_(5) {
}


NTRIPClient::~NTRIPClient() {
	Disconnect();
}


bool NTRIPClient::CreateSocket() {
	socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd_ < 0) {
		last_error_ = "Failed to create socket";
		return false;
	}

	timeval tv{};
	tv.tv_sec = connection_timeout_;
	tv.tv_usec = 0;
	setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	int flag = 1;
	setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));

	hostent *server = gethostbyname(host_.c_str());
	if (!server) {
		last_error_ = "Failed to resolve host: " + host_;
		close(socket_fd_);
		socket_fd_ = -1;
		return false;
	}

	sockaddr_in server_addr{};
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
	server_addr.sin_port = htons(port_);

	if (::connect(socket_fd_, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0) {
		last_error_ = "Failed to connect to " + host_ + ":" + std::to_string(port_);
		close(socket_fd_);
		socket_fd_ = -1;
		return false;
	}
	return true;
}


bool NTRIPClient::SendNTRIPRequest() {
	std::stringstream request;

	// Build HTTP GET request
	request << "GET /" << mountpoint_ << " HTTP/1.0\r\n";
	request << "User-Agent: " << user_agent_ << "\r\n";
	request << "Accept: */*\r\n";

	// Add Authorization header if credentials are provided
	if (!username_.empty() && !password_.empty()) {
		std::string auth = username_ + ":" + password_;
		request << "Authorization: Basic " << EncodeBase64(auth) << "\r\n";
	}

	request << "Connection: close\r\n";
	request << "\r\n";

	// Send request
	std::string request_str = request.str();
	if (send(socket_fd_, request_str.c_str(), request_str.length(), 0) < 0) {
		last_error_ = "Failed to send NTRIP request";
		return false;
	}

	return true;
}


bool NTRIPClient::ParseNTRIPResponse() {
	char buffer[1024];
	std::string response;

	while (response.find("\r\n\r\n") == std::string::npos) {
		int n = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);
		if (n <= 0) {
			last_error_ = "Failed to receive NTRIP response";
			return false;
		}
		buffer[n] = '\0';
		response += buffer;
	}

	if (response.find("HTTP/1.0 200 OK") != 0 && response.find("HTTP/1.1 200 OK") != 0) {
		if (response.find("401 Unauthorized") != std::string::npos) {
			last_error_ = "Authentication failed";
		} else if (response.find("404 Not Found") != std::string::npos) {
			last_error_ = "Mount point not found: " + mountpoint_;
		} else {
			last_error_ = "Invalid NTRIP response: " + response.substr(0, response.find("\r\n"));
		}
		return false;
	}
	return true;
}


bool NTRIPClient::Connect() {
	if (connected_) {
		return true;
	}

	if (!CreateSocket()) {
		return false;
	}

	if (!SendNTRIPRequest()) {
		close(socket_fd_);
		socket_fd_ = -1;
		return false;
	}

	if (!ParseNTRIPResponse()) {
		close(socket_fd_);
		socket_fd_ = -1;
		return false;
	}

	connected_ = true;
	start_time_ = std::chrono::steady_clock::now();
	return true;
}


void NTRIPClient::Disconnect() {
	StopReceiving();

	if (socket_fd_ >= 0) {
		close(socket_fd_);
		socket_fd_ = -1;
	}

	connected_ = false;
}


bool NTRIPClient::isConnected() const {
	return connected_;
}


void NTRIPClient::SetRTCMCallback(RTCMCallback callback) {
	rtcm_callback_ = callback;
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
	}
	if (process_thread_ && process_thread_->joinable()) {
		process_thread_->join();
	}
}


void NTRIPClient::ReceiveLoop() {
	std::vector<uint8_t> buffer(4096);
	std::vector<uint8_t> rtcm_buffer;
	rtcm_buffer.reserve(8192);

	while (receiving_ && connected_) {
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(socket_fd_, &read_fds);

		timeval tv{};
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		int ret = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
		if (ret < 0) {
			last_error_ = "Select error";
			break;
		} else if (ret == 0) {
			continue;
		}

		int n = recv(socket_fd_, buffer.data(), buffer.size(), 0);
		if (n <= 0) {
			if (n < 0) {
				last_error_ = "Receive error";
			} else {
				last_error_ = "Connection closed by server";
			}
			connected_ = false;

			if (auto_reconnect_) {
				HandleReconnection();
			}
			break;
		}

		bytes_received_ += n;

		rtcm_buffer.insert(rtcm_buffer.end(), buffer.begin(), buffer.begin() + n);

		size_t offset = 0;
		while (offset < rtcm_buffer.size()) {
			std::vector<uint8_t> message;
			if (ParseRTCMFrame(rtcm_buffer, offset, message)) {
				{
					std::lock_guard<std::mutex> lock(queue_mutex_);
					data_queue_.push(message);
				}
				queue_cv_.notify_one();
				messages_received_++;
			} else {
				break;
			}
		}

		if (offset > 0) {
			rtcm_buffer.erase(rtcm_buffer.begin(), rtcm_buffer.begin() + offset);
		}
	}
}


void NTRIPClient::ProcessLoop() {
	while (receiving_) {
		std::unique_lock<std::mutex> lock(queue_mutex_);

		queue_cv_.wait(lock, [this] {
			return !data_queue_.empty() || !receiving_;
		});

		while (!data_queue_.empty()) {
			std::vector<uint8_t> message = std::move(data_queue_.front());
			data_queue_.pop();
			lock.unlock();

			if (rtcm_callback_) {
				rtcm_callback_(message, message.size());
			}

			lock.lock();
		}
	}
}


bool NTRIPClient::ParseRTCMFrame(const std::vector<uint8_t> &buffer, size_t &offset, std::vector<uint8_t> &message) {
	// RTCM3消息格式：
	// Preamble (1 byte): 0xD3
	// Message Length (2 bytes): 6 reserved bits + 10 bits for length
	// Message (n bytes)
	// CRC-24 (3 bytes)

	// 查找前导码
	while (offset < buffer.size() && buffer[offset] != 0xD3) {
		offset++;
	}

	if (offset >= buffer.size()) {
		return false;
	}

	// 检查是否有足够的数据读取长度字段
	if (offset + 3 > buffer.size()) {
		return false;
	}

	// 读取消息长度（10位）
	uint16_t length = ((buffer[offset + 1] & 0x03) << 8) | buffer[offset + 2];

	// 检查是否有完整的消息
	size_t frame_size = 3 + length + 3; // Preamble + Length + Message + CRC
	if (offset + frame_size > buffer.size()) {
		return false;
	}

	// TODO: 验证CRC-24
	// 这里应该实现CRC-24验证，确保数据完整性

	// 提取完整消息
	message.assign(buffer.begin() + offset, buffer.begin() + offset + frame_size);
	offset += frame_size;

	return true;
}


void NTRIPClient::HandleReconnection() {
	while (auto_reconnect_ && receiving_) {
		std::this_thread::sleep_for(std::chrono::seconds(reconnect_interval_));

		std::cout << "Attempting to reconnect..." << std::endl;

		if (Connect()) {
			std::cout << "Reconnected successfully" << std::endl;
			return;
		}
	}
}


std::vector<MountPoint> NTRIPClient::GetSourceTable() {
	std::vector<MountPoint> mountpoints;

	if (!CreateSocket()) {
		return mountpoints;
	}

	std::stringstream request;
	request << "GET / HTTP/1.0\r\n";
	request << "User-Agent: " << user_agent_ << "\r\n";
	request << "Accept: */*\r\n";
	request << "Connection: close\r\n";
	request << "\r\n";

	std::string request_str = request.str();
	if (send(socket_fd_, request_str.c_str(), request_str.length(), 0) < 0) {
		close(socket_fd_);
		return mountpoints;
	}

	std::string response;
	char buffer[4096];
	int n;
	while ((n = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0)) > 0) {
		buffer[n] = '\0';
		response += buffer;
	}

	close(socket_fd_);
	socket_fd_ = -1;

	// 解析源表
	// TODO: 实现源表解析逻辑
	// 源表格式为SOURCETABLE格式，需要解析每一行

	return mountpoints;
}


std::string NTRIPClient::EncodeBase64(const std::string& input) {
	static const std::string base64_chars =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string encoded;
	int val = 0;
	int valb = -6;

	for (unsigned char c : input) {
		val = (val << 8) + c;
		valb += 8;
		while (valb >= 0) {
			encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
			valb -= 6;
		}
	}

	if (valb > -6) {
		encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
	}

	while (encoded.size() % 4) {
		encoded.push_back('=');
	}

	return encoded;
}


double NTRIPClient::GetDataRate() const {
	if (!connected_ || bytes_received_ == 0) {
		return 0.0;
	}
	auto now = std::chrono::steady_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
	if (duration == 0) {
		return 0.0;
	}
	return static_cast<double>(bytes_received_) / duration / 1024.0; // KB/s
}


void NTRIPClient::SetConnectionTimeout(int seconds) {
	connection_timeout_ = seconds;
}


void NTRIPClient::SetReceiveTimeout(int seconds) {
	receive_timeout_ = seconds;
}


void NTRIPClient::SetAutoReconnect(bool enable) {
	auto_reconnect_ = enable;
}


void NTRIPClient::SetReconnectInterval(int seconds) {
	reconnect_interval_ = seconds;
}
