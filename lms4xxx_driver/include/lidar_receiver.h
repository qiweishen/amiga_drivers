/// @file lidar_receiver.h
/// @brief SICK LMS4XXX scanner receiver — manages SICK API lifecycle, callbacks,
///        writer thread, and statistics. Analogous to INSDeviceReceiver.

#ifndef LIDAR_RECEIVER_H
#define LIDAR_RECEIVER_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "buffered_writer.h"
#include "lidar_data_type.h"
#include "point_cloud_callback.h"
#include "point_cloud_types.h"
#include "ring_buffer.h"


class LidarReceiver {
public:
    explicit LidarReceiver(LiDARConfig config);

    ~LidarReceiver();

    LidarReceiver(const LidarReceiver &) = delete;

    LidarReceiver &operator=(const LidarReceiver &) = delete;

    // Initialize: load SICK library, create API handle, init scanner.
    bool Init();

    // Start receiving: open writer, start writer thread, register callbacks.
    bool Start();

    // Stop: deregister callbacks, stop writer thread, close writer, release API.
    void Stop();

    [[nodiscard]] bool IsRunning() const { return running_.load(std::memory_order_acquire); }

    // Statistics (matching INS401 atomic counter pattern)
    struct Statistics {
        uint64_t frames_received = 0;
        uint64_t frames_written = 0;
        uint64_t frames_dropped = 0;
        uint64_t bytes_written = 0;
        uint64_t flush_count = 0;
    };

    [[nodiscard]] Statistics GetStatistics() const;

    void LogStatistics() const;

    [[nodiscard]] const std::string &OutputFile() const { return config_.output_file; }

private:
    LiDARConfig config_;

    // SICK API state
    void *api_handle_ = nullptr;
    bool lib_loaded_ = false;
    bool api_initialized_ = false;
    bool callback_registered_ = false;
    bool log_callback_registered_ = false;

    std::atomic<bool> running_{false};

    // Lock-free SPSC ring buffer: callback thread (producer) → writer thread (consumer).
    std::unique_ptr<Common::RingBuffer<PointCloudFrame> > ring_;
    CallbackContext callback_ctx_;
    std::unique_ptr<BufferedBinaryWriter> writer_;
    std::thread writer_thread_;

    // Statistics — lock-free atomics matching INS401 pattern.
    // frames_received and frames_dropped are in callback_ctx_ (incremented on SICK thread).
    // frames_written is tracked by the writer (single writer thread, no contention).
    std::atomic<uint64_t> stat_frames_written_{0};

    void WriterThreadFunc();

    void ReleaseApi();
};


#endif // LIDAR_RECEIVER_H
