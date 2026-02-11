#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "sick_scan_xd_api/sick_scan_api.h"



// Global state
static std::atomic<bool> g_terminate{false};
static std::atomic<uint64_t> g_dropped_frames{0};

static constexpr size_t kMaxQueueSize = 256;
static constexpr uint32_t kFileFormatVersion = 1;
static constexpr std::array<char, 8> kFileMagic = { 'L', 'I', 'D', 'A', 'R', 'P', 'C', 'D' };


// Point cloud data structures
struct PointXYZI {
    float x, y, z, intensity;
};


struct PointCloudFrame {
    uint64_t timestamp_ns;
    std::vector<PointXYZI> points;
};


// Thread-safe bounded queue
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t max_size)
        : max_size_(max_size) {
    }

    bool push(PointCloudFrame frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= max_size_) {
                return false;
            }
            queue_.push(std::move(frame));
        }
        cv_.notify_one();
        return true;
    }

    bool pop(PointCloudFrame &frame, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || g_terminate.load(); })) {
            return false;
        }
        if (queue_.empty()) {
            return false;
        }

        frame = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<PointCloudFrame> queue_;
    size_t max_size_;
};


// Queue instance
static ThreadSafeQueue g_queue(kMaxQueueSize);


// Signal handler
static void SignalHandler(int sig) {
    std::cerr << "\n[Signal] Received signal " << sig << ", shutting down...\n";
    g_terminate.store(true, std::memory_order_release);
}


// Point cloud callback (producer)
void pointCloudCallback(SickScanApiHandle apiHandle, const SickScanPointCloudMsg *msg) {
    (void) apiHandle;

    // Parse field offsets
    int offset_x = -1, offset_y = -1, offset_z = -1, offset_i = -1;
    for (int n = 0; n < msg->fields.size; n++) {
        const auto &field = msg->fields.buffer[n];
        if (field.datatype != SICK_SCAN_POINTFIELD_DATATYPE_FLOAT32)
            continue;

        if (strcmp(field.name, "x") == 0) {
            offset_x = field.offset;
        } else if (strcmp(field.name, "y") == 0) {
            offset_y = field.offset;
        } else if (strcmp(field.name, "z") == 0) {
            offset_z = field.offset;
        } else if (strcmp(field.name, "intensity") == 0 || strcmp(field.name, "i") == 0) {
            offset_i = field.offset;
        }
    }

    if (offset_x < 0 || offset_y < 0 || offset_z < 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::cerr << "[Callback] Missing required x/y/z fields, dropping frame\n";
        }
        return;
    }

    // Build a frame
    PointCloudFrame frame;
    frame.timestamp_ns = msg->header.timestamp_sec * 1'000'000'000ULL + msg->header.timestamp_nsec;
    frame.points.reserve(msg->width * msg->height);

    for (uint32_t row = 0; row < msg->height; ++row) {
        const uint8_t *row_ptr = msg->data.buffer + row * msg->row_step;
        for (uint32_t col = 0; col < msg->width; ++col) {
            const uint8_t *ptr = row_ptr + col * msg->point_step;
            PointXYZI pt;
            pt.x = *reinterpret_cast<const float *>(ptr + offset_x);
            pt.y = *reinterpret_cast<const float *>(ptr + offset_y);
            pt.z = *reinterpret_cast<const float *>(ptr + offset_z);
            pt.intensity = (offset_i >= 0) ? *reinterpret_cast<const float *>(ptr + offset_i) : 0.0f;

            if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.z)) {
                continue;
            }
            frame.points.push_back(pt);
        }
    }

    // Enqueue frame
    if (!g_queue.push(std::move(frame))) {
        g_dropped_frames.fetch_add(1, std::memory_order_relaxed);
    }
}


// Writer thread (consumer)
void writerThread(const std::string &output_path) {
    std::ofstream ofs(output_path, std::ios::binary);
    if (!ofs) {
        std::cerr << "[Writer] Failed to open: " << output_path << std::endl;
        return;
    }

    ofs.write(kFileMagic.data(), kFileMagic.size());
    ofs.write(reinterpret_cast<const char *>(&kFileFormatVersion), sizeof(kFileFormatVersion));

    size_t frames_written = 0;
    PointCloudFrame frame;

    while (!g_terminate.load(std::memory_order_acquire)) {
        if (g_queue.pop(frame, std::chrono::milliseconds(100))) {
            // Write timestamp
            ofs.write(reinterpret_cast<const char *>(&frame.timestamp_ns), sizeof(frame.timestamp_ns));

            // Write point count
            uint32_t num_points = static_cast<uint32_t>(frame.points.size());
            ofs.write(reinterpret_cast<const char *>(&num_points), sizeof(num_points));

            // Write points
            ofs.write(reinterpret_cast<const char *>(frame.points.data()), frame.points.size() * sizeof(PointXYZI));

            frames_written++;
            if (frames_written % 100 == 0) {
                ofs.flush();
                std::cout << "[Writer] Frames: " << frames_written
                        << ", Queue: " << g_queue.size()
                        << ", Dropped: " << g_dropped_frames.load() << std::endl;
            }
        }
    }

    // Drain remaining frames
    while (g_queue.pop(frame, std::chrono::milliseconds(0))) {
        ofs.write(reinterpret_cast<const char *>(&frame.timestamp_ns), sizeof(frame.timestamp_ns));
        uint32_t num_points = static_cast<uint32_t>(frame.points.size());
        ofs.write(reinterpret_cast<const char *>(&num_points), sizeof(num_points));
        ofs.write(reinterpret_cast<const char *>(frame.points.data()), frame.points.size() * sizeof(PointXYZI));
        frames_written++;
    }

    std::cout << "[Writer] Total frames written: " << frames_written
            << ", Dropped: " << g_dropped_frames.load() << std::endl;
}


// ============ 主函数 ============
int main(int argc, char *argv[]) {
    // Register signals
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Default args
    const char *launch_file = "../config/sick_lms_4xxx.launch";
    const char *output_file = "./pointcloud.bin";

    if (argc > 1) { launch_file = argv[1]; }
    if (argc > 2) { output_file = argv[2]; }

    // Load library
    std::vector<std::string> search_library_path = {
        "",
        "../3rd_party/FetchContent/sick_scan_xd/build/",
        "../3rd_party/FetchContent/sick_scan_xd/Release/"
    };
    const std::string lib_name = "libsick_scan_xd_shared_lib.so";
    bool lib_loaded = false;
    for (const auto &path: search_library_path) {
        const std::string lib_path = path + lib_name;
        if (SickScanApiLoadLibrary(lib_path.c_str()) == SICK_SCAN_API_SUCCESS) {
            std::cout << "[Main] Loaded library: " << lib_path << std::endl;
            lib_loaded = true;
            break;
        }
    }
    if (!lib_loaded) {
        std::cerr << "Failed to load library " << lib_name << std::endl;
        return 1;
    }

    // Create and initialize API
    const char *cli_args[] = {"app", launch_file};
    SickScanApiHandle apiHandle = SickScanApiCreate(2, const_cast<char **>(cli_args));
    if (!apiHandle) {
        std::cerr << "Failed to create api handle" << std::endl;
        SickScanApiUnloadLibrary();
        return 1;
    }

    int32_t init_ret = SickScanApiInitByLaunchfile(apiHandle, launch_file);
    if (init_ret != SICK_SCAN_API_SUCCESS) {
        init_ret = SickScanApiInitByCli(apiHandle, 2, const_cast<char **>(cli_args));
    }
    if (init_ret != SICK_SCAN_API_SUCCESS) {
        std::cerr << "Failed to initialize" << std::endl;
        SickScanApiRelease(apiHandle);
        SickScanApiUnloadLibrary();
        return 1;
    }

    // Start writer thread
    std::thread writer(writerThread, output_file);

    // Register callback
    if (SickScanApiRegisterCartesianPointCloudMsg(apiHandle, &pointCloudCallback) != SICK_SCAN_API_SUCCESS) {
        std::cerr << "Failed to register point cloud callback" << std::endl;
        g_terminate.store(true, std::memory_order_release);
        writer.join();
        SickScanApiClose(apiHandle);
        SickScanApiRelease(apiHandle);
        SickScanApiUnloadLibrary();
        return 1;
    }

    std::cout << "[Main] Running... Press Ctrl+C to stop" << std::endl;

    // Main loop
    while (!g_terminate.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Shutdown and cleanup
    std::cout << "[Main] Shutting down..." << std::endl;
    g_terminate.store(true, std::memory_order_release);
    SickScanApiDeregisterCartesianPointCloudMsg(apiHandle, &pointCloudCallback);

    // Wait for writer
    writer.join();

    SickScanApiClose(apiHandle);
    SickScanApiRelease(apiHandle);
    SickScanApiUnloadLibrary();

    return 0;
}
