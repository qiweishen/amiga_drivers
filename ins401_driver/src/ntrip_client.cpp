#include "ntrip_client.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "ethernet_socket.h"
#include "ins401_protocol.h"
#include "tool.h"


namespace {
    constexpr std::string_view kModule = "NTRIP Client";
    constexpr std::string_view kCallbackModule = "NTRIP Callback";
}


std::once_flag NTRIPClient::ssl_init_flag_;


NTRIPClient::NTRIPClient(const INIReader &configures) {
    LoadConfig(configures);
    // Reserve buffer space
    rtcm_buffer_.reserve(config_.max_buffer_size);
    if (config_.is_ssl) {
        // Initialize OpenSSL once
        std::call_once(ssl_init_flag_, InitOpenSSL);
    }
    // Initialize statistics timestamp
    stats_.last_message_time = std::chrono::steady_clock::now();
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
    // Check if already connected
    if (connected_.load(std::memory_order_acquire)) {
        return true;
    }
    // Create and connect socket
    if (!ConnectSocket()) {
        CloseConnection();
        return false;
    }
    // Initialize SSL if needed
    if (config_.is_ssl && !InitSSL()) {
        CloseConnection();
        return false;
    }
    // Send NTRIP request
    if (!SendRequest()) {
        CloseConnection();
        return false;
    }
    // Receive and validate response
    if (!ReceiveResponse()) {
        CloseConnection();
        return false;
    }
    // Connection successful
    connected_.store(true, std::memory_order_release);
    Tool::LogMessage(spdlog::level::info, kModule, fmt::format("Connected to {}:{} with mount point '{}' successfully",
                                                               config_.host, config_.port, config_.mount_point));
    // Reset statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.bytes_received = 0;
        stats_.messages_received = 0;
        stats_.messages_dropped = 0;
        stats_.crc_errors = 0;
        stats_.last_message_time = std::chrono::steady_clock::now();
    }
    return true;
}


void NTRIPClient::Disconnect() {
    // Stop receiving threads first
    StopReceiving();
    // Mark as disconnected
    connected_.store(false, std::memory_order_release);
    // Close network connection
    CloseConnection();
}


void NTRIPClient::StartReceiving() {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    // Check prerequisites
    if (!connected_.load(std::memory_order_acquire)) {
        Tool::LogMessage(spdlog::level::warn, kModule, "Cannot start receiving: not connected");
        return;
    }
    if (receiving_.load(std::memory_order_acquire)) {
        return; // Already receiving
    }
    // Set receiving flag
    receiving_.store(true, std::memory_order_release);
    // Clear data queue
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        while (!data_queue_.empty()) {
            data_queue_.pop();
        }
        if (!pending_data_.empty()) {
            data_queue_.push(std::move(pending_data_));
            pending_data_.clear();
        }
    }
    // Clear RTCM buffer
    rtcm_buffer_.clear();
    rtcm_sync_lost_count_ = 0;
    last_gga_sent_ = std::chrono::steady_clock::now() - std::chrono::seconds(config_.gga_interval);
    // Start threads
    receive_thread_ = std::make_unique<std::thread>(&NTRIPClient::ReceiveThread, this);
    process_thread_ = std::make_unique<std::thread>(&NTRIPClient::ProcessThread, this);
}


void NTRIPClient::StopReceiving() {
    std::unique_lock<std::mutex> lock(thread_mutex_);
    if (!receiving_.load(std::memory_order_acquire)) {
        return;
    }
    // Signal threads to stop
    receiving_.store(false, std::memory_order_release);
    // Wake up waiting threads
    queue_cv_.notify_all();
    // Move thread pointers to local variables
    std::unique_ptr<std::thread> recv_thread = std::move(receive_thread_);
    std::unique_ptr<std::thread> proc_thread = std::move(process_thread_);
    // Unlock before joining to avoid deadlock
    lock.unlock();
    // Wait for threads to complete
    if (recv_thread && recv_thread->joinable()) {
        recv_thread->join();
    }
    if (proc_thread && proc_thread->joinable()) {
        proc_thread->join();
    }
}


void NTRIPClient::LoadConfig(const INIReader &configures) {
    config_.host = configures.Get("NTRIP Client", "host", "");
    config_.port = static_cast<int>(configures.GetInteger("NTRIP Client", "port", 8080));
    config_.mount_point = configures.Get("NTRIP Client", "mount_point", "");
    config_.enable_vrs = configures.GetBoolean("NTRIP Client", "enable_vrs", false);
    config_.username = configures.Get("NTRIP Client", "username", "");
    config_.password = configures.Get("NTRIP Client", "password", "");
}


bool NTRIPClient::CreateSocket(const int family) {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    socket_fd_ = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd_ < 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to create socket: " + std::string(strerror(errno)));
        return false;
    }

    // RAII guard for automatic cleanup on failure
    Ethernet::FdGuard guard(socket_fd_);

    // Set socket options
    constexpr int reuse = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
        return false;
    }
    // TCP_NODELAY for low latency
    constexpr int nodelay = 1;
    if (setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to set TCP_NODELAY: " + std::string(strerror(errno)));
        return false;
    }
    // Set socket timeout
    timeval tv{};
    tv.tv_sec = config_.timeout;
    tv.tv_usec = 0;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to set receive timeout: " + std::string(strerror(errno)));
        return false;
    }
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to set send timeout: " + std::string(strerror(errno)));
        return false;
    }
    // Set keep-alive
    constexpr int keepalive = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to set keep-alive: " + std::string(strerror(errno)));
        return false;
    }
    // Set non-blocking mode
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to set non-blocking mode: " + std::string(strerror(errno)));
        return false;
    }

    guard.Release();
    return true;
}


bool NTRIPClient::ConnectSocket() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *result = nullptr;
    std::string port_str = std::to_string(config_.port);
    int ret = getaddrinfo(config_.host.c_str(), port_str.c_str(), &hints, &result);
    if (ret != 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to resolve hostname '" + config_.host + "': " + gai_strerror(ret));
        return false;
    }

    // RAII for addrinfo cleanup
    struct AddrInfoGuard {
        addrinfo *info;

        explicit AddrInfoGuard(addrinfo *ai) : info(ai) {
        }

        ~AddrInfoGuard() {
            if (info)
                freeaddrinfo(info);
        }
    } addr_guard(result);

    bool local_connected = false;
    for (const addrinfo *rp = result; rp != nullptr; rp = rp->ai_next) {
        if (!CreateSocket(rp->ai_family)) {
            continue;
        }
        ret = connect(socket_fd_, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            local_connected = true;
        } else if (errno == EINPROGRESS) {
            // handle non-blocking connect with select or poll
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(socket_fd_, &write_fds);
            timeval tv{};
            tv.tv_sec = config_.timeout;
            tv.tv_usec = 0;
            int sel = select(socket_fd_ + 1, nullptr, &write_fds, nullptr, &tv);
            if (sel > 0 && FD_ISSET(socket_fd_, &write_fds)) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) {
                    local_connected = true;
                }
            }
        }
        if (local_connected) {
            break;
        }
        close(socket_fd_);
        socket_fd_ = -1;
    }
    if (!local_connected) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Unable to connect to host '" + config_.host + "' on port " + port_str);
        return false;
    }
    return true;
}


bool NTRIPClient::InitSSL() {
    // Save & temporarily clear O_NONBLOCK
    const int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags == -1) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "F_GETFL failed: " + std::string(strerror(errno)));
        return false;
    }
    const bool was_nonblock = (flags & O_NONBLOCK) != 0;

    // RAII helper for restoring flags
    struct FlagRestorer {
        int fd;
        int flags;
        bool need_restore;

        ~FlagRestorer() {
            if (need_restore) {
                fcntl(fd, F_SETFL, flags);
            }
        }
    } restorer{socket_fd_, flags, was_nonblock};

    // Set to blocking mode for SSL handshake
    if (was_nonblock) {
        if (fcntl(socket_fd_, F_SETFL, flags & ~O_NONBLOCK) == -1) {
            Tool::LogMessage(spdlog::level::err, kModule,
                             "Failed to set blocking mode: " + std::string(strerror(errno)));
            return false;
        }
    }

    // SSL context
    const SSL_METHOD *method = TLS_client_method();
    ssl_ctx_.reset(SSL_CTX_new(method));
    if (!ssl_ctx_) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to create SSL context: " + GetSSLError());
        return false;
    }

    // Configure verification based on config
    if (config_.verify_ssl) {
        SSL_CTX_set_verify(ssl_ctx_.get(), SSL_VERIFY_PEER, nullptr);
        // Load trust anchors
        if (SSL_CTX_set_default_verify_paths(ssl_ctx_.get()) != 1) {
            const char *candidates[] = {
                "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu
                "/etc/pki/tls/certs/ca-bundle.crt", // RHEL/CentOS
                "/etc/ssl/cert.pem", // macOS/BSD
                "/etc/ssl/certs/ca-bundle.crt" // OpenSUSE
            };
            bool loaded = false;
            for (const auto *path: candidates) {
                if (access(path, R_OK) == 0 && SSL_CTX_load_verify_locations(ssl_ctx_.get(), path, nullptr) == 1) {
                    loaded = true;
                    break;
                }
            }
            if (!loaded) {
                Tool::LogMessage(spdlog::level::err, kModule,
                                 "Failed to load CA trust store from any known location");
                return false;
            }
        }
    } else {
        SSL_CTX_set_verify(ssl_ctx_.get(), SSL_VERIFY_NONE, nullptr);
    }

    // Create SSL connection
    ssl_.reset(SSL_new(ssl_ctx_.get()));
    if (!ssl_) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to create SSL connection: " + GetSSLError());
        return false;
    }

    // Configure SNI (Server Name Indication) and hostname verification
    if (!config_.host.empty()) {
        // Set SNI
        if (SSL_set_tlsext_host_name(ssl_.get(), config_.host.c_str()) != 1) {
            Tool::LogMessage(spdlog::level::err, kModule, "Failed to set SNI hostname");
            return false;
        }
        // Set hostname for certificate verification (only if verifying)
        if (config_.verify_ssl) {
            if (SSL_set1_host(ssl_.get(), config_.host.c_str()) != 1) {
                Tool::LogMessage(spdlog::level::err, kModule,
                                 "Failed to set hostname for verification");
                return false;
            }
        }
    }

    // Attach socket to SSL
    if (SSL_set_fd(ssl_.get(), socket_fd_) != 1) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         "Failed to set SSL socket: " + GetSSLError());
        return false;
    }
    // Set connection timeout
    timeval timeout{};
    timeout.tv_sec = config_.timeout;
    timeout.tv_usec = 0;
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Perform SSL handshake
    int ret = SSL_connect(ssl_.get());
    if (ret != 1) {
        const int ssl_error = SSL_get_error(ssl_.get(), ret);
        std::string error = "SSL handshake failed: " + GetSSLErrorString(ssl_error);
        // Additional debug info for verification failures
        if (config_.verify_ssl && ssl_error == SSL_ERROR_SSL) {
            long verify_result = SSL_get_verify_result(ssl_.get());
            if (verify_result != X509_V_OK) {
                error += " (Verify: " + std::string(X509_verify_cert_error_string(verify_result)) + ")";
            }
        }
        Tool::LogMessage(spdlog::level::err, kModule, error);
        return false;
    }
    return true;
}


bool NTRIPClient::SendRequest() const {
    std::string request = BuildHTTPRequest("/" + config_.mount_point);
    size_t total_sent = 0;
    while (total_sent < request.size()) {
        ssize_t sent = SendData(request.c_str() + total_sent, request.size() - total_sent);
        if (sent <= 0) {
            Tool::LogMessage(spdlog::level::err, kModule, "Failed to send request");
            return false;
        }
        total_sent += sent;
    }
    return true;
}


bool NTRIPClient::ReceiveResponse() {
    char buffer[4 * 1024];
    std::string response;

    // Waite data using select
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd_, &read_fds);
    timeval tv{};
    tv.tv_sec = config_.timeout;
    tv.tv_usec = 0;
    int sel = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
    if (sel <= 0) {
        Tool::LogMessage(spdlog::level::err, kModule,
                         (sel == 0) ? "Timeout waiting for response" : "Select error");
        CloseConnection();
        return false;
    }

    // Read until we have complete headers
    while (response.find("\r\n\r\n") == std::string::npos) {
        ssize_t received = ReceiveData(buffer, sizeof(buffer) - 1);
        if (received <= 0) {
            Tool::LogMessage(spdlog::level::err, kModule, "Failed to receive response");
            return false;
        }
        buffer[received] = '\0';
        response += buffer;
    }
    const auto header_end = response.find("\r\n\r\n");
    const std::string header_part = response.substr(0, header_end + 4);
    if (response.size() > header_end + 4) {
        const std::string body_part = response.substr(header_end + 4);
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        pending_data_.assign(body_part.begin(), body_part.end());
    }
    // Parse status code
    if (header_part.find("200 OK") == std::string::npos && header_part.find("ICY 200 OK") == std::string::npos) {
        if (header_part.find("401") != std::string::npos) {
            Tool::LogMessage(spdlog::level::err, kModule, "Authentication failed");
        } else if (header_part.find("404") != std::string::npos) {
            Tool::LogMessage(spdlog::level::err, kModule,
                             "Mount point not found: " + config_.mount_point);
        } else {
            Tool::LogMessage(spdlog::level::err, kModule, "HTTP error response");
        }
        return false;
    }
    return true;
}


std::vector<std::vector<uint8_t> > NTRIPClient::ParseRTCM(const uint8_t *data, size_t size) {
    std::vector<std::vector<uint8_t> > messages;
    if (size == 0) {
        return messages;
    }
    if (rtcm_buffer_.size() + size > config_.max_buffer_size) {
        rtcm_buffer_.clear();
        rtcm_sync_lost_count_++;
    }

    // Add new data to buffer
    rtcm_buffer_.insert(rtcm_buffer_.end(), data, data + size);

    size_t messages_received = 0;
    while (rtcm_buffer_.size() >= 1024) {
        std::vector<uint8_t> msg(rtcm_buffer_.begin(), rtcm_buffer_.begin() + 1024);
        messages.push_back(std::move(msg));
        rtcm_buffer_.erase(rtcm_buffer_.begin(), rtcm_buffer_.begin() + 1024);
        messages_received++;
    }
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.messages_received += messages_received;
    }
    return messages;
}


void NTRIPClient::ReceiveThread() {
    uint8_t buffer[64 * 1024];
    while (receiving_.load(std::memory_order_acquire) && connected_.load(std::memory_order_acquire)) {
        if (config_.enable_vrs) {
            SendGgaIfNeeded();
        }
        ssize_t received = ReceiveData(buffer, sizeof(buffer));
        if (received > 0) {
            // Update statistics
            {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.bytes_received += received;
                stats_.last_message_time = std::chrono::steady_clock::now();
            }
            // Add to queue
            std::vector<uint8_t> data(buffer, buffer + received);
            {
                std::lock_guard<std::mutex> queue_lock(queue_mutex_);
                // Check queue size limit
                if (data_queue_.size() >= config_.max_queue_size) {
                    // Drop oldest data
                    data_queue_.pop();
                    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                    stats_.messages_dropped++;
                }
                data_queue_.push(std::move(data));
            }
            queue_cv_.notify_one();
        } else if (received == 0) {
            // Connection closed by remote
            connected_.store(false, std::memory_order_release);
            Tool::LogMessage(spdlog::level::warn, kModule, "Connection closed by remote host");
            if (config_.auto_reconnect && receiving_.load(std::memory_order_acquire)) {
                HandleReconnect();
            }
            break;
        } else {
            // Handle errors
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT) {
                connected_.store(false, std::memory_order_release);
                Tool::LogMessage(spdlog::level::err, kModule,
                                 "Receive error: " + std::string(strerror(errno)));
                if (config_.auto_reconnect && receiving_.load(std::memory_order_acquire)) {
                    HandleReconnect();
                }
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}


void NTRIPClient::ProcessThread() {
    while (receiving_.load(std::memory_order_acquire)) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            // Wait for data or stop signal
            queue_cv_.wait(lock, [this] {
                return !data_queue_.empty() || !receiving_.load(std::memory_order_acquire);
            });
            if (!receiving_.load(std::memory_order_acquire) && data_queue_.empty())
                break;
            data = std::move(data_queue_.front());
            data_queue_.pop();
        }
        auto messages = ParseRTCM(data.data(), data.size());

        std::function<void(const uint8_t *, size_t)> data_cb;
        std::function<void(const std::vector<uint8_t> &)> msg_cb;
        {
            std::lock_guard<std::mutex> lk(callback_mutex_);
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
}


void NTRIPClient::HandleReconnect() {
    int attempts = 0;
    int current_interval = config_.reconnect_interval;
    while (config_.auto_reconnect && receiving_.load(std::memory_order_acquire) && attempts < config_.
           max_reconnect_attempts) {
        attempts++;
        // Update statistics
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.reconnect_count++;
        }
        // Wait before reconnection attempt
        std::this_thread::sleep_for(std::chrono::seconds(current_interval));
        // Check if still need to reconnect
        if (!receiving_.load(std::memory_order_acquire)) {
            break;
        }
        Tool::LogMessage(spdlog::level::info, kModule,
                         "Reconnection attempt " + std::to_string(attempts) + "/" +
                         std::to_string(config_.max_reconnect_attempts));
        // Close existing connection
        CloseConnection();
        // Attempt reconnection
        if (Connect()) {
            Tool::LogMessage(spdlog::level::info, kModule,
                             "Reconnected successfully after " + std::to_string(attempts) + " attempts");
            return;
        }
        // Exponential backoff
        if (config_.exponential_backoff && attempts > 3) {
            current_interval = std::min(current_interval * 2, 60);
        }
    }
    // Reconnection failed
    Tool::LogMessage(spdlog::level::err, kModule,
                     "Failed to reconnect after " + std::to_string(attempts) + " attempts");
    receiving_.store(false, std::memory_order_release);
}


ssize_t NTRIPClient::SendData(const void *data, size_t size) const {
    ssize_t result;
    if (ssl_) {
        result = SSL_write(ssl_.get(), data, static_cast<int>(size));
        if (result <= 0) {
            int ssl_error = SSL_get_error(ssl_.get(), static_cast<int>(result));
            if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
                Tool::LogMessage(spdlog::level::err, kModule,
                                 "SSL write error: " + std::to_string(ssl_error));
            }
        }
    } else {
        result = send(socket_fd_, data, size, MSG_NOSIGNAL);
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            Tool::LogMessage(spdlog::level::err, kModule,
                             "Socket send error: " + std::string(strerror(errno)));
        }
    }
    return result;
}


ssize_t NTRIPClient::ReceiveData(void *buffer, size_t size) const {
    ssize_t result;
    if (ssl_) {
        result = SSL_read(ssl_.get(), buffer, static_cast<int>(size));
        if (result <= 0) {
            int ssl_error = SSL_get_error(ssl_.get(), static_cast<int>(result));
            if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
                Tool::LogMessage(spdlog::level::err, kModule,
                                 "SSL read error: " + std::to_string(ssl_error));
            }
        }
    } else {
        result = recv(socket_fd_, buffer, size, 0);
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT) {
            Tool::LogMessage(spdlog::level::err, kModule,
                             "Socket recv error: " + std::string(strerror(errno)));
        }
    }
    return result;
}


void NTRIPClient::CloseConnection() {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    connected_.store(false, std::memory_order_release);
    // Close SSL connection
    if (ssl_) {
        SSL_shutdown(ssl_.get());
        ssl_.reset();
    }
    if (ssl_ctx_) {
        ssl_ctx_.reset();
    }
    // Close socket
    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
}


void NTRIPClient::SendGgaIfNeeded() {
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::seconds(std::max(1, config_.gga_interval));
    if (now - last_gga_sent_ < interval) {
        return;
    }
    std::string gga;
    {
        std::lock_guard<std::mutex> lock(gga_mutex_);
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
    if (SendData(gga.data(), gga.size()) > 0) {
        last_gga_sent_ = now;
    } else {
        Tool::LogMessage(spdlog::level::warn, kModule, "Failed to send GGA for VRS");
    }
}


void NTRIPClient::UpdateGgaFromNmea(std::string nmea) {
    if (nmea.empty()) {
        return;
    }
    if (nmea.find("$GPGGA,") != 0 && nmea.find("$GNGGA,") != 0) {
        return;
    }
    SetNmeaGga(std::move(nmea));
}


std::vector<NTRIPClient::MountPoint> NTRIPClient::GetSourceTable() {
    std::vector<MountPoint> result;
    if (!ConnectSocket()) {
        return result;
    }
    if (config_.is_ssl && !InitSSL()) {
        CloseConnection();
        return result;
    }

    // Send request for source table (empty path)
    const std::string request = BuildHTTPRequest("/");
    if (SendData(request.c_str(), request.size()) <= 0) {
        CloseConnection();
        return result;
    }

    // Waite data using select
    std::string response;
    char buffer[64 * 1024]; // 64 KB buffer
    fd_set readfds;
    timeval tv{};

    while (true) {
        FD_ZERO(&readfds);
        FD_SET(socket_fd_, &readfds);
        tv.tv_sec = config_.timeout;
        tv.tv_usec = 0;
        int ret = select(socket_fd_ + 1, &readfds, nullptr, nullptr, &tv);
        if (ret <= 0)
            break; // timeout or error
        ssize_t received = ReceiveData(buffer, sizeof(buffer) - 1);
        if (received <= 0)
            break;
        buffer[received] = '\0';
        response.append(buffer, received);
    }

    // Parse source table
    std::istringstream stream(response);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("STR;") == 0) {
            // Parse mount point line
            MountPoint mount_point;
            std::vector<std::string_view> fields = Tool::Utility::SplitString(line, ';');
            // Parse SOURCETABLE fields
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
                mount_point.carrier = std::stoi(fields[5].data());
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
                mount_point.latitude = std::stod(fields[9].data());
            }
            if (fields.size() > 10) {
                mount_point.longitude = std::stod(fields[10].data());
            }
            result.push_back(mount_point);
        }
    }
    CloseConnection();
    return result;
}


NTRIPClient::Statistics NTRIPClient::GetStatistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    Statistics result = stats_;
    // Calculate current data rate
    const auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - stats_.last_message_time).count();
    if (duration > 0 && stats_.bytes_received > 0) {
        result.current_data_rate = static_cast<double>(stats_.bytes_received) / static_cast<double>(duration) / 1024.0;
        // KB/s
    }
    return result;
}


std::string NTRIPClient::GetSSLError() {
    const unsigned long err = ERR_get_error();
    if (err == 0) {
        return "Unknown SSL error";
    }
    char err_buf[256];
    ERR_error_string_n(err, err_buf, sizeof(err_buf));
    return err_buf;
}


std::string NTRIPClient::GetSSLErrorString(int ssl_error) {
    switch (ssl_error) {
        case SSL_ERROR_NONE:
            return "No error";
        case SSL_ERROR_ZERO_RETURN:
            return "SSL connection closed";
        case SSL_ERROR_WANT_READ:
            return "SSL wants read";
        case SSL_ERROR_WANT_WRITE:
            return "SSL wants write";
        case SSL_ERROR_WANT_CONNECT:
            return "SSL wants connect";
        case SSL_ERROR_SYSCALL:
            return "SSL syscall error: " + std::string(strerror(errno));
        case SSL_ERROR_SSL:
            return "SSL protocol error: " + GetSSLError();
        default:
            return "Unknown SSL error code: " + std::to_string(ssl_error);
    }
}


std::string NTRIPClient::BuildHTTPRequest(const std::string &path) const {
    std::stringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: " << config_.host << ":" << config_.port << "\r\n";
    request << "User-Agent: " << config_.user_agent << "\r\n";
    request << "Accept: */*\r\n";
    request << "Connection: keep-alive\r\n";
    request << "Ntrip-Version: Ntrip/2.0\r\n";
    // Authentication
    if (!config_.username.empty() && !config_.password.empty()) {
        const std::string credentials = config_.username + ":" + config_.password;
        request << "Authorization: Basic " << Base64Encode(credentials) << "\r\n";
    }
    // NMEA GGA
    std::string gga;
    {
        std::lock_guard<std::mutex> lock(gga_mutex_);
        gga = nmea_gga_;
    }
    if (!gga.empty()) {
        request << "Ntrip-GGA: " << gga << "\r\n";
    }
    request << "\r\n";
    return request.str();
}


std::string NTRIPClient::Base64Encode(const std::string &input) {
    BUF_MEM *buffer_ptr;

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, input.c_str(), static_cast<int>(input.length()));
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &buffer_ptr);

    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);

    return result;
}


NTRIP_Callback::NTRIP_Callback(const std::string &interface, const std::string &target_mac_str,
                               const std::string &local_mac_str) : interface_(interface) {
    target_mac_ = Ethernet::FormatMACAddress(target_mac_str);
    local_mac_ = Ethernet::FormatMACAddress(local_mac_str);
}


NTRIP_Callback::~NTRIP_Callback() {
    Cleanup();
}


bool NTRIP_Callback::IsInitialized() const {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    return socket_initialized_;
}


bool NTRIP_Callback::Initialize() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_initialized_ && socket_ptr_ && socket_ptr_->IsValid()) {
        return true;
    }
    try {
        socket_ptr_ = std::make_shared<EthernetSocket>(interface_, target_mac_);
        socket_initialized_ = socket_ptr_->IsValid();
    } catch (const std::exception &e) {
        Tool::LogMessage(spdlog::level::err, kCallbackModule,
                         "Failed to initialize socket on " + interface_ + ": " + e.what());
        socket_initialized_ = false;
        socket_ptr_.reset();
    }
    return socket_initialized_;
}


void NTRIP_Callback::Cleanup() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    socket_ptr_.reset();
    socket_initialized_ = false;
}


void NTRIP_Callback::Reset() {
    Cleanup();
    Initialize();
}


bool NTRIP_Callback::SendToINS401(const uint8_t *payload, size_t payload_length) {
    if (!Initialize()) {
        packets_failed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MacAddress local_mac = local_mac_;
    if (socket_ptr_ && socket_ptr_->IsValid()) {
        local_mac = socket_ptr_->GetLocalMac();
    }
    // Build Ethernet packet
    std::vector<uint8_t> rtcm_base_packet;
    try {
        rtcm_base_packet = Ethernet::BuildAceinnaPacket(target_mac_, local_mac, RTCM_BASE_DATA_MESSAGE_ID_BYTES,
                                                        payload, payload_length);
    } catch (const std::exception &e) {
        Tool::LogMessage(spdlog::level::err, kCallbackModule,
                         "Packet build error: " + std::string(e.what()));
        packets_failed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Send packet with retry
    bool sent = false;
    for (int retry = 0; retry < MAX_SEND_RETRIES_; retry++) {
        const auto send_result = socket_ptr_ ? socket_ptr_->Send(rtcm_base_packet) : -1;
        if (send_result > 0) {
            sent = true;
            break;
        }
        if (retry < MAX_SEND_RETRIES_ - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SEND_RETRY_DELAY_MS_));
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
