#include "lms4xxx_driver_app.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "driver_markers.h"
#include "lms4xxx_tool.h"
#include "logger.h"
#include "utility.h"


namespace {
    Common::DriverLog g_log{std::string(Common::Markers::kModuleLms4xxx)};

    // Main loop polling interval
    constexpr auto kPollInterval = std::chrono::milliseconds(200);

    // "hh:mm:ss"
    std::string HumanDuration(std::uint64_t seconds) {
        return fmt::format("{:02}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    }

    // DriverStatistics::NtpStatus -> status-line token
    const char *NtpStatusText(std::uint8_t status) {
        switch (status) {
            case LMS4xxx::DriverStatistics::kNtpOk:
                return "ok";
            case LMS4xxx::DriverStatistics::kNtpNoTimestamp:
                return "NO-TS";
            case LMS4xxx::DriverStatistics::kNtpUnreachable:
                return "UNREACH";
            default:
                return "off";
        }
    }

    // Adaptive "N.NN GiB/MiB/KiB"
    std::string HumanBytes(std::uint64_t bytes) {
        constexpr double kKiB = 1024.0;
        const auto b = static_cast<double>(bytes);
        if (b >= kKiB * kKiB * kKiB) {
            return fmt::format("{:.2f} GiB", b / (kKiB * kKiB * kKiB));
        }
        if (b >= kKiB * kKiB) {
            return fmt::format("{:.2f} MiB", b / (kKiB * kKiB));
        }
        if (b >= kKiB) {
            return fmt::format("{:.2f} KiB", b / kKiB);
        }
        return fmt::format("{} B", bytes);
    }

    // Build channel mask from scan config.
    std::uint16_t BuildChannelMask(const LMS4xxx::ScanConfig &scan) {
        std::uint16_t mask = 0;
        if (scan.enable_distance) {
            mask |= LMS4xxx::ChannelMask::kDist1;
        }
        if (scan.enable_rssi) {
            mask |= LMS4xxx::ChannelMask::kRssi1;
        }
        if (scan.enable_reflectance) {
            mask |= LMS4xxx::ChannelMask::kRefl1;
        }
        if (scan.enable_angle_correction) {
            mask |= LMS4xxx::ChannelMask::kAngl1;
        }
        if (scan.enable_quality) {
            mask |= LMS4xxx::ChannelMask::kQlty1;
        }
        return mask;
    }
} // namespace


Lms4xxxDriverApp::Lms4xxxDriverApp(LiDARConfig config) : impl_(std::make_unique<Impl>()), config_(std::move(config)) {
}


Lms4xxxDriverApp::~Lms4xxxDriverApp() {
    if (impl_->driver) {
        shutdown();
    }
}


bool Lms4xxxDriverApp::init(const std::function<bool()> & /*external_stop*/) {
    impl_->instance_name = config_.position_name.empty() ? config_.hostname : config_.position_name;

    // Apply hostname override to driver config
    if (!config_.hostname.empty()) {
        config_.driver_config.device.ip = config_.hostname;
    }
    // Instance tag: every driver-internal log line becomes "[<instance>] ..."
    config_.driver_config.name = impl_->instance_name;

    // Map LiDARConfig NTP fields to DriverConfig.ntp — unconditionally, so an
    // empty yaml server or a bad interval fails Validate() instead of silently
    // falling back to compiled-in defaults
    config_.driver_config.ntp.enable = config_.enable_ntp;
    config_.driver_config.ntp.server_ip = config_.ntp_server_ip;
    config_.driver_config.ntp.update_interval_s =
            config_.sync_time > 0.0 ? static_cast<std::uint32_t>(std::lround(config_.sync_time)) : 0;
    config_.driver_config.ntp.check_status_s =
            config_.ntp_check_status_s > 0.0 ? static_cast<std::uint32_t>(std::lround(config_.ntp_check_status_s)) : 0;

    auto ec = config_.driver_config.Validate();
    if (ec) {
        g_log.error("[{}] Invalid configuration: {}", impl_->instance_name, ec.message());
        return false;
    }

    impl_->driver = std::make_unique<LMS4xxx::LMS4xxxDriver>(config_.driver_config);

    impl_->driver->SetErrorCallback([name = impl_->instance_name](std::error_code err, const std::string &detail) {
        g_log.error("[{}] Error: {}{}", name, err.message(), detail.empty() ? "" : " (" + detail + ")");
    });

    if (!config_.data_folder_path.empty()) {
        LMS4xxx::ScanRecordWriter::Config writer_config;
        writer_config.bin_path = fmt::format("{}/bin/lms4xxx/scan_{}_{}.bin", config_.data_folder_path,
                                             impl_->instance_name, config_.timestamp);
        writer_config.instance = impl_->instance_name;
        writer_config.channel_mask = BuildChannelMask(config_.driver_config.scan);
        writer_config.queue_capacity = config_.recording_queue_capacity;
        writer_config.write_buffer_size = config_.recording_write_buffer_size;
        writer_config.max_file_bytes = config_.recording_max_file_bytes;

        impl_->writer = std::make_unique<LMS4xxx::ScanRecordWriter>(writer_config);
        impl_->driver->SetScanCallback([writer = impl_->writer.get()](const LMS4xxx::ScanData &scan) {
            writer->OnScan(scan);
        });

        g_log.trace("[{}] Scan recording enabled (channels: 0x{:02X})", impl_->instance_name,
                    writer_config.channel_mask);
    } else {
        g_log.error("[{}] Cannot set up the scan writer (empty data folder path)", impl_->instance_name);
        return false;
    }

    ec = impl_->driver->Connect();
    if (ec) {
        g_log.error("[{}] Connect failed: {}", impl_->instance_name, ec.message());
        return false;
    }

    ec = impl_->driver->Configure();
    if (ec) {
        g_log.error("[{}] Configure failed: {}", impl_->instance_name, ec.message());
        impl_->driver->Disconnect();
        return false;
    }

    g_log.info(fmt::runtime(Common::Markers::kLmsInitializedInstTpl), impl_->instance_name);
    return true;
}


void Lms4xxxDriverApp::run() {
    if (!impl_->driver) {
        g_log.error(true, "run() called without init()");
        return;
    }

    // Start scan record writer before scanning so no frames are missed
    if (impl_->writer && !impl_->writer->Start(config_.driver_config.scan)) {
        terminate_.store(true, std::memory_order_release);
        g_log.error(true, "[{}] Cannot start the scan writer: recording is mandatory for a run",
                    impl_->instance_name);
        return; // unreachable
    }

    auto ec = impl_->driver->StartScanning();
    if (ec) {
        // Clean up BEFORE the throw: error(true, ...) never returns
        if (impl_->writer) {
            impl_->writer->Stop();
        }
        terminate_.store(true, std::memory_order_release);
        g_log.error(true, "[{}] Start scanning failed: {}", impl_->instance_name, ec.message());
        return; // unreachable; keeps the control flow obvious
    }

    g_log.info("[{}] Scanning started", impl_->instance_name);

    // Main loop: wait for termination, periodically log the status line
    using clock = std::chrono::steady_clock;
    const double stats_interval = config_.stats_interval_s;
    impl_->scan_start = clock::now();
    auto last_stats = impl_->scan_start;
    std::uint64_t rate_prev_frames = 0;
    auto rate_prev_time = impl_->scan_start;

    while (!terminate_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kPollInterval);

        // Check if driver is still healthy
        if (!impl_->driver->IsScanning()) {
            g_log.warn("[{}] Scanning stopped unexpectedly", impl_->instance_name);
            terminate_.store(true, std::memory_order_release);
            break;
        }
        // Fatal fault (first-scan verification failure / NTP clock wrong /
        // receive-channel error): data collected past it is invalid, so the
        // run terminates instead of silently recording on. The specific cause
        // was already reported at error level via the driver's error callback.
        if (impl_->driver->HasFault()) {
            g_log.error("[{}] Fatal driver fault — terminating the run (see the error above for the cause)",
                        impl_->instance_name);
            terminate_.store(true, std::memory_order_release);
            break;
        }

        const auto now = clock::now();
        if (stats_interval > 0 && std::chrono::duration<double>(now - last_stats).count() >= stats_interval) {
            last_stats = now;
            const auto drv = impl_->driver->GetStatistics();
            const LMS4xxx::ScanRecordWriter::Statistics wr =
                    impl_->writer ? impl_->writer->GetStatistics() : LMS4xxx::ScanRecordWriter::Statistics{};
            // Sensor's actual scan rate since the previous status line (the
            // device streams one CoLa B frame per scan)
            const double dt = std::chrono::duration<double>(now - rate_prev_time).count();
            const double scan_hz = dt > 0.0
                                       ? static_cast<double>(drv.frames_received - rate_prev_frames) / dt
                                       : 0.0;
            rate_prev_frames = drv.frames_received;
            rate_prev_time = now;
            const auto uptime_s = static_cast<std::uint64_t>(std::chrono::duration<double>(now - impl_->scan_start).
                count());
            g_log.info("[Statistics] [{}] up={}  rate={:.1f} Hz  ntp={}  frames={}  parsed={}  drop_ring={}  gaps={}  "
                       "crc={}  frame_err={}  parse_err={}  written={}  drop_q={}  files={}  bytes={}",
                       impl_->instance_name, HumanDuration(uptime_s), scan_hz, NtpStatusText(drv.ntp_status),
                       drv.frames_received, drv.frames_parsed, drv.frames_dropped, drv.counter_gaps, drv.crc_errors,
                       drv.framing_errors, drv.parse_errors, wr.frames_written, wr.frames_dropped, wr.files_created,
                       HumanBytes(wr.bytes_written));
        }
    }

    g_log.trace("[{}] Run() exiting", impl_->instance_name);
}


void Lms4xxxDriverApp::shutdown() {
    if (!impl_->driver) {
        return;
    }

    if (impl_->driver->IsScanning()) {
        impl_->driver->StopScanning();
    }

    // Stop recording first: flush remaining frames and close the binary file,
    // so the final summary reports the on-disk totals
    if (impl_->writer) {
        impl_->writer->Stop();
    }

    const auto drv = impl_->driver->GetStatistics();
    const LMS4xxx::ScanRecordWriter::Statistics wr =
            impl_->writer ? impl_->writer->GetStatistics() : LMS4xxx::ScanRecordWriter::Statistics{};
    const auto duration_s = impl_->scan_start == std::chrono::steady_clock::time_point{}
                                ? 0ull
                                : static_cast<std::uint64_t>(std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - impl_->scan_start)
                                    .count());
    g_log.info("[Statistics] [{}] Final: duration={}  frames={}  parsed={}  delivery={:.1f}%  ntp={}  "
               "dropped_ring={}  counter_gaps={}  crc_errors={}  framing_errors={}  parse_errors={}  "
               "frames_written={}  dropped_queue={}  bytes={}  files={}",
               impl_->instance_name, HumanDuration(duration_s), drv.frames_received, drv.frames_parsed,
               drv.DeliveryRate(), NtpStatusText(drv.ntp_status), drv.frames_dropped, drv.counter_gaps,
               drv.crc_errors, drv.framing_errors, drv.parse_errors, wr.frames_written, wr.frames_dropped,
               HumanBytes(wr.bytes_written), wr.files_created);

    impl_->driver->Disconnect();

    g_log.info(fmt::runtime(Common::Markers::kLmsShutdownInstTpl), impl_->instance_name);

    // Reset driver so destructor and repeated shutdown() calls are no-ops.
    impl_->driver.reset();
}
