#include "ins401_ntrip_client.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

#include "ins401_ethernet_socket.h"
#include "ins401_protocol.h"
#include "ins401_tool.h"
#include "logger.h"
#include "string_util.h"
#include "utility.h"


namespace INS401 {
namespace {
	constexpr std::string_view kModule = "NTRIPClient";
	constexpr std::string_view kCallbackModule = "NTRIPCallback";
	Common::DriverLog g_log{ std::string(kModule) };
	Common::DriverLog g_callback_log{ std::string(kCallbackModule) };
}  // namespace


NTRIPClient::NTRIPClient(const INSConfig &config) {
	LoadConfig(config);
	rtcm_buffer_.reserve(config_.max_buffer_size);
	stats_.last_message_time = std::chrono::steady_clock::now();
}

NTRIPClient::~NTRIPClient() {
	Disconnect();
}


bool NTRIPClient::Connect() {
	if (connected_.load(std::memory_order_acquire)) {
		return true;
	}
	if (!ConnectSocket()) {
		CloseConnection();
		return false;
	}
	if (!SendRequest()) {
		CloseConnection();
		return false;
	}
	if (!ReceiveResponse()) {
		CloseConnection();
		return false;
	}
	connected_.store(true, std::memory_order_release);
	disconnected_.store(false, std::memory_order_release);
	g_log.info("Connected to NTRIP caster {}:{} with mount point '{}' successfully", config_.host, config_.port,
			   config_.mount_point);
	{
		std::scoped_lock lock(stats_mutex_);
		stats_.bytes_received = 0;
		stats_.messages_received = 0;
		stats_.messages_dropped = 0;
		stats_.crc_errors = 0;
		stats_.last_message_time = std::chrono::steady_clock::now();
	}
	return true;
}


void NTRIPClient::Disconnect() {
	if (disconnected_.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	StopReceiving();
	connected_.store(false, std::memory_order_release);
	CloseConnection();
	g_log.info("Disconnected from NTRIP caster {}:{} with mount point '{}'", config_.host, config_.port, config_.mount_point);
	const Statistics final_stats = GetStatistics();
	if (final_stats.bytes_received > 0) {
		g_log.info("=== NTRIP STATISTICS ===  Total bytes received: {}", final_stats.bytes_received);
		g_log.info("=== NTRIP STATISTICS ===  Total messages processed: {}", final_stats.messages_received);
		g_log.info("=== NTRIP STATISTICS ===  Number of reconnections: {}", final_stats.reconnect_count);
		g_log.info("=== NTRIP STATISTICS ===  Received RTCM CRC errors: {}", final_stats.crc_errors);
	}
}


void NTRIPClient::StartReceiving() {
	std::scoped_lock lock(thread_mutex_);
	if (!connected_.load(std::memory_order_acquire)) {
		g_log.warn("Cannot start receiving: not connected");
		return;
	}
	if (receiving_.load(std::memory_order_acquire)) {
		return;
	}
	if (stopping_.load(std::memory_order_acquire) || receive_thread_ || process_thread_) {
		g_log.warn("Cannot start receiving: previous session still shutting down");
		return;
	}
	receiving_.store(true, std::memory_order_release);
	// Fresh queue for this receiving session; hand over any body bytes that
	// arrived together with the HTTP response
	queue_ = std::make_unique<DataQueue>(config_.max_queue_size);
	{
		std::scoped_lock pending_lock(pending_mutex_);
		if (!pending_data_.empty()) {
			(void) queue_->try_push(std::move(pending_data_));
			pending_data_.clear();
		}
	}

	rtcm_buffer_.clear();
	rtcm_sync_lost_count_ = 0;
	last_gga_sent_ = std::chrono::steady_clock::now() - std::chrono::seconds(config_.gga_interval);

	receive_thread_ = std::make_unique<std::thread>(&NTRIPClient::ReceiveThread, this);
	process_thread_ = std::make_unique<std::thread>(&NTRIPClient::ProcessThread, this);
}


void NTRIPClient::StopReceiving() {
	std::unique_lock<std::mutex> lock(thread_mutex_);
	// No early-exit on !receiving_: a thread that died on its own (exception,
	// exhausted reconnects) leaves receiving_ false with joinable threads
	// behind — skipping the joins here would std::terminate in ~NTRIPClient
	stopping_.store(true, std::memory_order_release);
	receiving_.store(false, std::memory_order_release);
	if (queue_) {
		queue_->close();
	}
	std::unique_ptr<std::thread> recv_thread = std::move(receive_thread_);
	std::unique_ptr<std::thread> proc_thread = std::move(process_thread_);
	// Unlock before joining to avoid deadlock
	lock.unlock();
	// Wait for threads to complete (join on an already-exited thread returns
	// immediately)
	if (recv_thread && recv_thread->joinable()) {
		recv_thread->join();
	}
	if (proc_thread && proc_thread->joinable()) {
		proc_thread->join();
	}
	stopping_.store(false, std::memory_order_release);
}


void NTRIPClient::LoadConfig(const INSConfig &config) {
	config_.enable_rtk = config.enable_rtk;
	config_.host = config.host;
	config_.port = config.port;
	config_.mount_point = config.mount_point;
	config_.use_vrs = config.use_vrs;
	config_.username = config.username;
	config_.password = config.password;
}


bool NTRIPClient::ConnectSocket() {
	std::scoped_lock lock(connection_mutex_);
	Common::TcpClient::Options opts;
	opts.host = config_.host;
	opts.port = static_cast<std::uint16_t>(config_.port);
	opts.tcp_keepalive = true;
	// SO_RCVTIMEO doubles as the idle tick for the receive loop (GGA timing,
	// stop-flag checks)
	opts.recv_timeout_ms = config_.timeout * 1000;
	opts.send_timeout_ms = config_.timeout * 1000;
	tcp_ = std::make_unique<Common::TcpClient>(std::move(opts));

	if (auto ec = tcp_->Connect(config_.timeout * 1000)) {
		LogErrorOrWarn("Unable to connect to host '" + config_.host + "' on port " + std::to_string(config_.port) + " - " +
					   ec.message());
		return false;
	}
	return true;
}


bool NTRIPClient::SendRequest() const {
	const std::string request = BuildHTTPRequest("/" + config_.mount_point);
	if (!SendData(request.c_str(), request.size())) {
		LogErrorOrWarn("Failed to send request");
		return false;
	}
	return true;
}


bool NTRIPClient::ReceiveResponse() {
	char buffer[4 * 1024];
	std::string response;

	// Read until we have complete headers (ReceiveData returns 0 on idle
	// timeout; give the caster a bounded number of idle ticks)
	int idle_ticks = 0;
	while (response.find("\r\n\r\n") == std::string::npos) {
		std::error_code ec;
		const size_t received = ReceiveData(buffer, sizeof(buffer) - 1, ec);
		if (ec) {
			LogErrorOrWarn("Failed to receive response");
			return false;
		}
		if (received == 0) {
			if (++idle_ticks >= 3) {
				LogErrorOrWarn("Timeout waiting for response");
				return false;
			}
			continue;
		}
		idle_ticks = 0;
		response.append(buffer, received);
	}
	const auto header_end = response.find("\r\n\r\n");
	const std::string header_part = response.substr(0, header_end + 4);
	// Parse status code FIRST (NTRIP v1 casters answer with the non-standard
	// "ICY 200 OK"): an error body (401/404 HTML) must never reach the RTCM path
	if (header_part.find("200 OK") == std::string::npos && header_part.find("ICY 200 OK") == std::string::npos) {
		{
			std::scoped_lock pending_lock(pending_mutex_);
			pending_data_.clear();
		}
		if (header_part.find("401") != std::string::npos) {
			LogErrorOrWarn("Authentication failed");
		} else if (header_part.find("404") != std::string::npos) {
			LogErrorOrWarn("Mount point not found: " + config_.mount_point);
		} else {
			LogErrorOrWarn("HTTP error response");
		}
		return false;
	}
	if (response.size() > header_end + 4) {
		// Body bytes that arrived with the headers: deliver to the active
		// queue (reconnect) or park them for StartReceiving (first connect)
		std::vector<uint8_t> body(response.begin() + static_cast<std::ptrdiff_t>(header_end) + 4, response.end());
		if (receiving_.load(std::memory_order_acquire) && queue_) {
			EnqueueReceived(std::move(body));
		} else {
			std::scoped_lock pending_lock(pending_mutex_);
			pending_data_ = std::move(body);
		}
	}
	return true;
}


// Buffer incoming RTCM data and emit 1024-byte chunks.
// 1024 bytes fits within the Ethernet MTU for raw socket forwarding to INS401.
std::vector<std::vector<uint8_t> > NTRIPClient::ChunkRTCMData(const uint8_t *data, size_t size) {
	std::vector<std::vector<uint8_t> > messages;
	if (size == 0) {
		return messages;
	}
	if (rtcm_buffer_.size() + size > config_.max_buffer_size) {
		rtcm_buffer_.clear();
		rtcm_sync_lost_count_++;
	}

	rtcm_buffer_.insert(rtcm_buffer_.end(), data, data + size);

	size_t messages_received = 0;
	while (rtcm_buffer_.size() >= 1024) {
		std::vector<uint8_t> msg(rtcm_buffer_.begin(), rtcm_buffer_.begin() + 1024);
		messages.push_back(std::move(msg));
		rtcm_buffer_.erase(rtcm_buffer_.begin(), rtcm_buffer_.begin() + 1024);
		messages_received++;
	}
	{
		std::scoped_lock stats_lock(stats_mutex_);
		stats_.messages_received += messages_received;
	}
	return messages;
}


void NTRIPClient::EnqueueReceived(std::vector<uint8_t> data) {
	// Atomic drop-oldest keeps fresh corrections when the queue is full and
	// refuses cleanly once the queue is closed (shutdown window)
	bool dropped = false;
	if (queue_->push_drop_oldest(std::move(data), dropped) && dropped) {
		std::scoped_lock stats_lock(stats_mutex_);
		stats_.messages_dropped++;
	}
}


void NTRIPClient::ReceiveThread() {
	try {
		uint8_t buffer[64 * 1024];
		while (receiving_.load(std::memory_order_acquire) && connected_.load(std::memory_order_acquire)) {
			if (config_.use_vrs) {
				SendGGA();
			}
			std::error_code ec;
			const size_t received = ReceiveData(buffer, sizeof(buffer), ec);
			if (received > 0) {
				{
					std::scoped_lock stats_lock(stats_mutex_);
					stats_.bytes_received += received;
					stats_.last_message_time = std::chrono::steady_clock::now();
				}
				EnqueueReceived(std::vector<uint8_t>(buffer, buffer + received));
			} else if (ec) {
				// Connection lost (remote close/reset) or hard error
				connected_.store(false, std::memory_order_release);
				if (ec == Common::TcpError::kConnectionLost) {
					g_log.warn("Connection closed by remote host");
				} else {
					LogErrorOrWarn("Receive error: " + ec.message());
				}
				if (config_.auto_reconnect && receiving_.load(std::memory_order_acquire)) {
					HandleReconnect();
					if (connected_.load(std::memory_order_acquire)) {
						continue;  // reconnected: resume receiving on the fresh socket
					}
				}
				break;
			}
			// received == 0 without error: SO_RCVTIMEO idle tick — loop re-checks
			// the stop flag and the GGA timer
		}
	} catch (const std::exception &e) {
		connected_.store(false, std::memory_order_release);
		receiving_.store(false, std::memory_order_release);
		g_log.error("ReceiveThread exception: {}", e.what());
		if (queue_) {
			queue_->close();
		}
	}
}


void NTRIPClient::ProcessThread() {
	try {
		std::vector<uint8_t> data;
		while (queue_->pop(data)) {	 // blocks; drains after close()
			auto messages = ChunkRTCMData(data.data(), data.size());

			std::function<void(const uint8_t *, size_t)> data_cb;
			std::function<void(const std::vector<uint8_t> &)> msg_cb;
			{
				std::scoped_lock lk(callback_mutex_);
				data_cb = data_callback_;
				msg_cb = message_callback_;
			}
			for (const auto &msg: messages) {
				if (!receiving_.load(std::memory_order_acquire)) {
					break;
				}
				if (data_cb) {
					data_cb(msg.data(), msg.size());
				}
				if (msg_cb) {
					msg_cb(msg);
				}
			}
		}
	} catch (const std::exception &e) {
		receiving_.store(false, std::memory_order_release);
		g_log.error("ProcessThread exception: {}", e.what());
	}
}


// Reconnect with exponential backoff: starts at reconnect_interval, doubles after
// 3 consecutive failures, capped at 60s. Gives up after max_reconnect_attempts.
void NTRIPClient::HandleReconnect() {
	int attempts = 0;
	int current_interval = config_.reconnect_interval;
	while (config_.auto_reconnect && receiving_.load(std::memory_order_acquire) && attempts < config_.max_reconnect_attempts) {
		attempts++;
		{
			std::scoped_lock stats_lock(stats_mutex_);
			stats_.reconnect_count++;
		}
		std::this_thread::sleep_for(std::chrono::seconds(current_interval));
		if (!receiving_.load(std::memory_order_acquire)) {
			break;
		}
		g_log.info("Reconnection attempt {}/{}", attempts, config_.max_reconnect_attempts);
		CloseConnection();
		if (Connect()) {
			g_log.info("Reconnected successfully after {} attempts", attempts);
			return;
		}
		if (config_.exponential_backoff && attempts > 3) {
			current_interval = std::min(current_interval * 2, 60);
		}
	}
	LogErrorOrWarn("Failed to reconnect after " + std::to_string(attempts) + " attempts");
	receiving_.store(false, std::memory_order_release);
	if (queue_) {
		queue_->close();
	}
}


bool NTRIPClient::SendData(const void *data, size_t size) const {
	std::scoped_lock lock(connection_mutex_);
	if (!tcp_) {
		return false;
	}
	if (auto ec = tcp_->Write(static_cast<const std::uint8_t *>(data), size)) {
		g_log.warn("Socket send error: {}", ec.message());
		return false;
	}
	return true;
}


size_t NTRIPClient::ReceiveData(void *buffer, size_t size, std::error_code &ec) const {
	Common::TcpClient *tcp = nullptr;
	{
		std::scoped_lock lock(connection_mutex_);
		tcp = tcp_.get();
	}
	if (!tcp) {
		ec = Common::make_error_code(Common::TcpError::kNotConnected);
		return 0;
	}
	// ReadSome: > 0 data; 0 with clear ec = SO_RCVTIMEO idle tick; ec set on
	// connection loss or error
	return tcp->ReadSome(static_cast<std::uint8_t *>(buffer), size, ec);
}


void NTRIPClient::CloseConnection() {
	std::scoped_lock lock(connection_mutex_);
	connected_.store(false, std::memory_order_release);
	if (tcp_) {
		tcp_->Disconnect();
		tcp_.reset();
	}
}


void NTRIPClient::SendGGA() {
	const auto now = std::chrono::steady_clock::now();
	const auto interval = std::chrono::seconds(std::max(1, config_.gga_interval));
	if (now - last_gga_sent_ < interval) {
		return;
	}
	std::string gga;
	{
		std::scoped_lock lock(gga_mutex_);
		gga = nmea_gga_;
	}
	if (gga.empty()) {
		last_gga_sent_ = now;
		return;
	}
	if (gga.back() != '\n') {
		if (gga.back() == '\r') {
			gga.push_back('\n');
		} else {
			gga.append("\r\n");
		}
	}
	if (SendData(gga.data(), gga.size())) {
		last_gga_sent_ = now;
	} else {
		g_log.warn("Failed to send GGA for VRS");
	}
}


std::vector<NTRIPClient::MountPoint> NTRIPClient::GetSourceTable() {
	std::vector<MountPoint> result;
	if (receiving_.load(std::memory_order_acquire) || connected_.load(std::memory_order_acquire)) {
		// ConnectSocket would replace tcp_ under the active session's feet
		g_log.warn("GetSourceTable refused: client is connected/receiving");
		return result;
	}
	if (!ConnectSocket()) {
		return result;
	}

	// Send request for source table (empty path)
	const std::string request = BuildHTTPRequest("/");
	if (!SendData(request.c_str(), request.size())) {
		CloseConnection();
		return result;
	}

	// Read until the caster stops sending (idle timeout) or closes
	std::string response;
	char buffer[64 * 1024];
	while (true) {
		std::error_code ec;
		const size_t received = ReceiveData(buffer, sizeof(buffer) - 1, ec);
		if (ec || received == 0) {
			break;	// closed, error or idle timeout
		}
		response.append(buffer, received);
	}

	std::istringstream stream(response);
	std::string line;
	while (std::getline(stream, line)) {
		if (line.find("STR;") == 0) {
			MountPoint mount_point;
			std::vector<std::string> fields = Tool::Utility::SplitString(line, ';');
			if (fields.size() > 1) {
				mount_point.mount_point = fields[1];
			}
			if (fields.size() > 2) {
				mount_point.city = fields[2];
			}
			if (fields.size() > 3) {
				mount_point.data_format = fields[3];
			}
			if (fields.size() > 4) {
				mount_point.format_details = fields[4];
			}
			if (fields.size() > 5) {
				mount_point.carrier = std::stoi(fields[5]);
			}
			if (fields.size() > 6) {
				mount_point.nav_system = fields[6];
			}
			if (fields.size() > 7) {
				mount_point.network = fields[7];
			}
			if (fields.size() > 8) {
				mount_point.country = fields[8];
			}
			if (fields.size() > 9) {
				mount_point.latitude = std::stod(fields[9]);
			}
			if (fields.size() > 10) {
				mount_point.longitude = std::stod(fields[10]);
			}
			result.push_back(mount_point);
		}
	}
	CloseConnection();
	return result;
}


bool NTRIPClient::IsRTKRequired() const {
	return config_.enable_rtk;
}


void NTRIPClient::LogErrorOrWarn(std::string_view msg) const {
	if (config_.enable_rtk) {
		Common::Log::log_and_throw(kModule, msg);
	}
	g_log.warn("{} (RTK disabled, ignored)", msg);
}


NTRIPClient::Statistics NTRIPClient::GetStatistics() const {
	std::scoped_lock lock(stats_mutex_);
	Statistics result = stats_;
	const auto now = std::chrono::steady_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - stats_.last_message_time).count();
	if (duration > 0 && stats_.bytes_received > 0) {
		result.current_data_rate = static_cast<double>(stats_.bytes_received) / static_cast<double>(duration) / 1024.0;
		// KB/s
	}
	return result;
}


std::string NTRIPClient::BuildHTTPRequest(const std::string &path) const {
	std::stringstream request;
	request << "GET " << path << " HTTP/1.1\r\n";
	request << "Host: " << config_.host << ":" << config_.port << "\r\n";
	request << "User-Agent: " << config_.user_agent << "\r\n";
	request << "Accept: */*\r\n";
	request << "Connection: keep-alive\r\n";
	request << "Ntrip-Version: Ntrip/2.0\r\n";
	if (!config_.username.empty() && !config_.password.empty()) {
		const std::string credentials = config_.username + ":" + config_.password;
		request << "Authorization: Basic " << Base64Encode(credentials) << "\r\n";
	}
	std::string gga;
	{
		std::scoped_lock lock(gga_mutex_);
		gga = nmea_gga_;
	}
	if (!gga.empty()) {
		request << "Ntrip-GGA: " << gga << "\r\n";
	}
	request << "\r\n";
	return request.str();
}


std::string NTRIPClient::Base64Encode(const std::string &input) {
	return Common::StringUtil::Base64Encode(input);
}


NTRIPCallback::NTRIPCallback(std::string interface, std::string target_mac_str, std::string local_mac_str) :
	interface_(std::move(interface)) {
	target_mac_ = Ethernet::FormatMACAddress(std::move(target_mac_str));
	local_mac_ = Ethernet::FormatMACAddress(std::move(local_mac_str));
}


NTRIPCallback::~NTRIPCallback() {
	Cleanup();
}


bool NTRIPCallback::IsInitialized() const {
	std::scoped_lock lock(socket_mutex_);
	return socket_initialized_;
}


bool NTRIPCallback::Initialize() {
	std::scoped_lock lock(socket_mutex_);
	if (socket_initialized_ && socket_ptr_ && socket_ptr_->IsValid()) {
		return true;
	}
	try {
		socket_ptr_ = std::make_shared<EthernetSocket>(interface_, target_mac_);
		socket_initialized_ = socket_ptr_->IsValid();
	} catch (const std::exception &e) {
		g_callback_log.warn("Failed to initialize socket on {}: {}", interface_, e.what());
		socket_initialized_ = false;
		socket_ptr_.reset();
	}
	return socket_initialized_;
}


void NTRIPCallback::Cleanup() {
	std::scoped_lock lock(socket_mutex_);
	socket_ptr_.reset();
	socket_initialized_ = false;
}


void NTRIPCallback::Reset() {
	Cleanup();
	Initialize();
}


bool NTRIPCallback::SendToINS401(const uint8_t *payload, size_t payload_length) {
	if (!Initialize()) {
		packets_failed_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	MacAddress local_mac = local_mac_;
	if (socket_ptr_ && socket_ptr_->IsValid()) {
		local_mac = socket_ptr_->GetLocalMac();
	}
	std::vector<uint8_t> rtcm_base_packet;
	try {
		rtcm_base_packet =
				Ethernet::BuildAceinnaPacket(target_mac_, local_mac, RTCM_BASE_DATA_MESSAGE_ID_BYTES, payload, payload_length);
	} catch (const std::exception &e) {
		g_callback_log.warn("Packet build error: {}", e.what());
		packets_failed_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	bool sent = false;
	for (int retry = 0; retry < kMaxSendRetries; retry++) {
		const auto send_result = socket_ptr_ ? socket_ptr_->Send(rtcm_base_packet) : -1;
		if (send_result > 0) {
			sent = true;
			break;
		}
		if (retry < kMaxSendRetries - 1) {
			std::this_thread::sleep_for(std::chrono::milliseconds(kSendRetryDelayMs));
		}
	}
	if (sent) {
		packets_sent_.fetch_add(1, std::memory_order_relaxed);
		return true;
	} else {
		packets_failed_.fetch_add(1, std::memory_order_relaxed);

		// Reset socket on persistent failures
		if (packets_failed_.load(std::memory_order_relaxed) % 10 == 0) {
			Reset();
		}
		return false;
	}
}
}  // namespace INS401
