/// @file lidar_receiver.cpp
/// @brief SICK LMS4XXX scanner receiver implementation
///
/// Encapsulates the SICK scan_xd library lifecycle (load, init, handle, callbacks)
/// and the writer thread that drains point cloud frames from the callback queue
/// to disk via BufferedBinaryWriter

#include "lidar_receiver.h"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <utility>
#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include "utility.h"

#include "sick_scan_xd_api/sick_scan_api.h"


namespace {
    constexpr std::string_view kModule = "LidarReceiver";
}


// RAII helper: captures stdout into a pipe while in scope, then logs to the
// file-only "sick" logger on destruction.  The SICK library uses raw printf()
// in some code paths (wrapper, init, close), so the verbose-level API alone
// isn't enough to silence all console output
class CaptureStdout {
public:
    explicit CaptureStdout(bool active) : active_(active) {
        if (!active_) return;
        if (::pipe(pipe_fd_) != 0) return;
        std::fflush(stdout);
        saved_fd_ = ::dup(STDOUT_FILENO);
        ::dup2(pipe_fd_[1], STDOUT_FILENO);   // stdout → pipe write end
        ::close(pipe_fd_[1]);                  // close extra write-end fd
        pipe_fd_[1] = -1;
    }

    ~CaptureStdout() {
        if (!active_ || saved_fd_ < 0) return;
        // Restore original stdout (closes the pipe write end)
        std::fflush(stdout);
        ::dup2(saved_fd_, STDOUT_FILENO);
        ::close(saved_fd_);

        // Read all captured output (write end is closed, read will reach EOF)
        if (pipe_fd_[0] >= 0) {
            std::string captured;
            char buf[4096];
            ssize_t n;
            while ((n = ::read(pipe_fd_[0], buf, sizeof(buf))) > 0) {
                captured.append(buf, n);
            }
            ::close(pipe_fd_[0]);

            // Forward each line to the file-only sick logger
            if (!captured.empty()) {
                if (auto logger = spdlog::get("sick")) {
                    std::istringstream stream(captured);
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (!line.empty()) {
                            logger->info("[SICK]: {}", line);
                        }
                    }
                    logger->flush();
                }
            }
        }
    }

    CaptureStdout(const CaptureStdout &) = delete;
    CaptureStdout &operator=(const CaptureStdout &) = delete;

private:
    bool active_ = false;
    int saved_fd_ = -1;
    int pipe_fd_[2] = {-1, -1};
};


// SICK log message callback: forwards to our shared logger
void sickLogCallback(SickScanApiHandle /*apiHandle*/, const SickScanLogMsg *msg) {
    if (msg && msg->log_message) {
        Common::Log::sick_msg(msg->log_level, msg->log_message);
    }
}


LidarReceiver::LidarReceiver(LiDARConfig config)
    : config_(std::move(config)) {
}


LidarReceiver::~LidarReceiver() {
    Stop();
}


bool LidarReceiver::Init() {
    // Load the SICK scan library
    {
        CaptureStdout guard(config_.quiet);
        for (const auto &path: config_.library_search_paths) {
            std::filesystem::path exe_dir = Common::GetExecutableDir(); // exe_dir + "../../" -> project root
            std::string lib_path = (exe_dir / "../../" / path / config_.library_name).string();
            if (SickScanApiLoadLibrary(lib_path.c_str()) == SICK_SCAN_API_SUCCESS) {
                lib_loaded_ = true;
                break;
            }
        }
    }
    if (!lib_loaded_) {
        Common::Log::log_message(spdlog::level::warn, kModule,
                                 fmt::format("Failed to load library {}", config_.library_name));
        return false;
    }
    Common::Log::log_message(spdlog::level::info, kModule,
                             fmt::format("Loaded library: {}", config_.library_name));

    // Build CLI args for the SICK API
    std::vector<std::string> arg_strings = {"app", config_.launch_file};
    // Apply generic launch parameter overrides from YAML
    for (const auto& [key, value] : config_.launch_overrides) {
        arg_strings.push_back(key + ":=" + value);
    }
    // NTP server (from separate YAML section)
    if (!config_.ntp_server_ip.empty()) {
        arg_strings.push_back("ntp_server_address:=" + config_.ntp_server_ip);
        Common::Log::log_message(spdlog::level::info, kModule,
                                 fmt::format("NTP server: {}", config_.ntp_server_ip));
    }
    std::vector<char *> cli_args;
    cli_args.reserve(arg_strings.size());
    for (auto &s: arg_strings) {
        cli_args.push_back(s.data());
    }
    int cli_argc = static_cast<int>(cli_args.size());

    // Create the API handle (SICK library prints verbose parameter list to stdout)
    {
        CaptureStdout guard(config_.quiet);
        api_handle_ = SickScanApiCreate(cli_argc, cli_args.data());
    }
    if (!api_handle_) {
        Common::Log::log_message(spdlog::level::warn, kModule, "Failed to create API handle");
        return false;
    }

    // Configure SICK logging (before init, which is very chatty)
    // Suppress SICK's own console output by setting verbose level high
    SickScanApiSetVerboseLevel(api_handle_, 5);

    // Register our callback to capture SICK messages into the shared log file
    if (SickScanApiRegisterLogMsg(api_handle_, &sickLogCallback) == SICK_SCAN_API_SUCCESS) {
        log_callback_registered_ = true;
    } else {
        Common::Log::log_message(spdlog::level::warn, kModule, "Failed to register SICK log callback");
    }

    // Initialize the scanner
    int32_t init_ret;
    {
        CaptureStdout guard(config_.quiet);
        init_ret = SickScanApiInitByCli(api_handle_, cli_argc, cli_args.data());
    }
    if (init_ret != SICK_SCAN_API_SUCCESS) {
        Common::Log::log_message(spdlog::level::warn, kModule, "Failed to initialize scanner");
        return false;
    }
    api_initialized_ = true;

    Common::Log::log_message(spdlog::level::info, kModule, "Scanner initialized");
    return true;
}


bool LidarReceiver::Start() {
    // Create the lock-free SPSC ring buffer (capacity rounded to power of 2)
    ring_ = std::make_unique<Common::RingBuffer<PointCloudFrame> >(config_.max_queue_size);

    // Create and open the buffered writer
    BufferedWriterConfig writer_config;
    writer_config.output_path = config_.output_file;
    writer_config.buffer_size = config_.write_buffer_size;
    writer_config.status_interval_frames = config_.status_interval_frames;

    writer_ = std::make_unique<BufferedBinaryWriter>(writer_config);
    if (!writer_->open()) {
        Common::Log::log_message(spdlog::level::warn, kModule, "Failed to open output file");
        return false;
    }

    // Start the writer thread
    running_.store(true, std::memory_order_release);
    writer_thread_ = std::thread(&LidarReceiver::WriterThreadFunc, this);

    // Set up callback context and register
    callback_ctx_.ring = ring_.get();
    callback_ctx_.frames_received.store(0, std::memory_order_relaxed);
    callback_ctx_.dropped_frames.store(0, std::memory_order_relaxed);
    setCallbackContext(&callback_ctx_);

    if (SickScanApiRegisterCartesianPointCloudMsg(api_handle_, &pointCloudCallback) != SICK_SCAN_API_SUCCESS) {
        Common::Log::log_message(spdlog::level::warn, kModule,
                                 "Failed to register point cloud callback");
        running_.store(false, std::memory_order_release);
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
        clearCallbackContext();
        return false;
    }
    callback_registered_ = true;

    Common::Log::log_message(spdlog::level::info, kModule, "Receiving data... Press Ctrl+C to stop");
    return true;
}


void LidarReceiver::Stop() {
    // Idempotent: safe to call multiple times

    // 1. Deregister callbacks (stops new data from arriving)
    if (log_callback_registered_ && api_handle_) {
        SickScanApiDeregisterLogMsg(api_handle_, &sickLogCallback);
        log_callback_registered_ = false;
    }
    if (callback_registered_ && api_handle_) {
        SickScanApiDeregisterCartesianPointCloudMsg(api_handle_, &pointCloudCallback);
        callback_registered_ = false;
    }
    clearCallbackContext();

    // 2. Signal termination to writer thread
    running_.store(false, std::memory_order_release);

    // 3. Wait for the writer thread to drain the ring buffer and finish
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    // 4. Close the writer (flushes remaining buffered data to disk)
    if (writer_) {
        writer_->close();
        writer_.reset();
    }

    // 5. Release SICK API resources
    ReleaseApi();

    ring_.reset();
}


void LidarReceiver::ReleaseApi() {
    CaptureStdout guard(config_.quiet);
    if (api_initialized_ && api_handle_) {
        SickScanApiClose(api_handle_);
        api_initialized_ = false;
    }
    if (api_handle_) {
        SickScanApiRelease(api_handle_);
        api_handle_ = nullptr;
    }
    if (lib_loaded_) {
        SickScanApiUnloadLibrary();
        lib_loaded_ = false;
    }
}


LidarReceiver::Statistics LidarReceiver::GetStatistics() const {
    Statistics s;
    s.frames_received = callback_ctx_.frames_received.load(std::memory_order_relaxed);
    s.frames_dropped = callback_ctx_.dropped_frames.load(std::memory_order_relaxed);
    s.frames_written = stat_frames_written_.load(std::memory_order_relaxed);
    if (writer_) {
        WriterStats ws = writer_->stats();
        s.bytes_written = ws.bytes_written;
        s.flush_count = ws.flush_count;
    }
    return s;
}


void LidarReceiver::LogStatistics() const {
    const Statistics s = GetStatistics();

    Common::Log::log_message(spdlog::level::info, kModule, fmt::format(
                                 "=== LMS4XXX RECEIVER STATISTICS ==="));
    Common::Log::log_message(spdlog::level::info, kModule, fmt::format(
                                 "  Frames received: {}", s.frames_received));
    Common::Log::log_message(spdlog::level::info, kModule, fmt::format(
                                 "  Frames written:  {}", s.frames_written));
    Common::Log::log_message(spdlog::level::info, kModule, fmt::format(
                                 "  Frames dropped:  {}", s.frames_dropped));
    Common::Log::log_message(spdlog::level::info, kModule, fmt::format(
                                 "  Bytes written:   {}", s.bytes_written));
    Common::Log::log_message(spdlog::level::info, kModule, fmt::format(
                                 "  Flushes:         {}", s.flush_count));
}


void LidarReceiver::WriterThreadFunc() {
    try {
        PointCloudFrame frame;

        while (running_.load(std::memory_order_acquire)) {
            if (ring_->try_pop(frame)) {
                if (!writer_->write_frame(frame)) {
                    Common::Log::log_message(spdlog::level::warn, kModule,
                                             "Write error, stopping writer thread");
                    break;
                }
                stat_frames_written_.fetch_add(1, std::memory_order_relaxed);

                // Periodic status to log
                uint64_t written = stat_frames_written_.load(std::memory_order_relaxed);
                if (config_.status_interval_frames > 0 &&
                    written % config_.status_interval_frames == 0) {
                    Common::Log::log_message(spdlog::level::info, kModule, fmt::format(
                                                 "Frames: {}, Ring: ~{}/{}, Dropped: {}",
                                                 written, ring_->size(), ring_->capacity(),
                                                 callback_ctx_.dropped_frames.load(std::memory_order_relaxed)));
                }
            } else {
                // Ring buffer empty — brief sleep to avoid busy-spinning
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }

        // Drain remaining frames from the ring buffer after stop signal
        while (ring_->try_pop(frame)) {
            if (!writer_->write_frame(frame)) {
                Common::Log::log_message(spdlog::level::warn, kModule,
                                         "Write error during drain");
                break;
            }
            stat_frames_written_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (const std::exception &e) {
        // Use spdlog::error() directly — Common::Log::log_message(err) would re-throw
        spdlog::error("[LidarReceiver] Writer thread exception: {}", e.what());
        running_.store(false, std::memory_order_release);
    }
}
