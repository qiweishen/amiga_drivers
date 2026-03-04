/// @file lidar_driver_app.h
/// @brief Top-level application class managing the SICK LMS 4xxx driver lifecycle
///
/// Encapsulates config loading, LidarReceiver management, and graceful shutdown
/// with optional post-processing (binary→CSV). Matches InsDriverApp lifecycle pattern

#ifndef DRIVER_APP_H
#define DRIVER_APP_H


#include <atomic>
#include <memory>
#include <string>

#include "data_type.h"
#include "lidar_data_type.h"

class LidarReceiver;


class LidarDriverApp {
public:
    // Unified main path: stores config paths, loads LiDARConfig in init()
    explicit LidarDriverApp(const Common::Config &config);

    // Standalone main path: config already fully populated
    explicit LidarDriverApp(LiDARConfig config);

    ~LidarDriverApp();

    LidarDriverApp(const LidarDriverApp &) = delete;

    LidarDriverApp &operator=(const LidarDriverApp &) = delete;

    bool init();

    void run();

    void shutdown();

    // Called from signal handlers (async-signal-safe: only atomic store)
    void request_shutdown();

    std::atomic<bool> &terminate_flag();

private:
    std::atomic<bool> terminate_{false};
    std::atomic<bool> shutdown_called_{false};

    // Config loading state (unified main path)
    std::string config_path_;
    std::string launch_file_override_; // From Common::Config
    std::string data_folder_path_; // From Common::Config
    std::string timestamp_; // From Common::Config
    bool config_preloaded_ = false; // True when constructed with LiDARConfig directly

    LiDARConfig config_{};

    // Core receiver (owns SICK API, queue, writer thread, statistics)
    std::unique_ptr<LidarReceiver> receiver_;
};


#endif // DRIVER_APP_H
