#include "scan_record_writer.h"

#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

#include "app_config.h"
#include "scan_h5_file.h"
#include "logger.h"
#include "ring_buffer.h"


namespace {
    constexpr std::string_view kModule = "LMS4xxx";
    common::DriverLog g_log{std::string(kModule)};
    constexpr auto kWriteBackoffSleep = std::chrono::microseconds(500);

    // Attribute size guard for the embedded config snapshot
    constexpr std::size_t kMaxConfigYamlBytes = 256 * 1024;


    std::string ReadTextFile(const std::string &path, std::size_t max_bytes) {
        if (path.empty()) {
            return {};
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return {};
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.size() > max_bytes) {
            return {};
        }
        return text;
    }
} // namespace


namespace lms4xxx {
    struct ScanRecordWriter::Impl {
        Config config;
        std::string config_yaml;
        std::uint32_t record_bytes = 0;

        // SPSC: parse thread -> write thread
        std::unique_ptr<common::RingBuffer<ScanRecord> > queue;

        std::thread write_thread;
        std::atomic<bool> running{false};
        std::atomic<bool> failed{false};

        // Owned by the write thread between Start() and Stop()
        ScanH5File file;
        std::mutex file_mutex; // split rollover vs telemetry rows
        std::uint32_t split_index = 0;
        std::uint64_t file_bytes = 0;
        bool compression_warned = false;

        std::atomic<std::size_t> stat_frames_written{0};
        std::atomic<std::size_t> stat_frames_dropped{0};
        std::atomic<std::size_t> stat_bytes_written{0};
        std::atomic<std::size_t> stat_files_created{0};
        std::atomic<std::size_t> stat_frames_queued{0};

        explicit Impl(Config cfg) : config(std::move(cfg)) {
        }

        // scan_xxx.h5 -> scan_xxx_000.h5, _001.h5, ...
        [[nodiscard]] std::string SplitPath(std::uint32_t index) const {
            const std::filesystem::path p(config.output_path);
            const std::string filename = fmt::format("{}_{:03d}{}", p.stem().string(), index, p.extension().string());
            return (p.parent_path() / filename).string();
        }

        [[nodiscard]] bool OpenSplit() {
            ScanH5File::Options opts;
            opts.path = SplitPath(split_index);
            opts.instance = config.instance;
            opts.session_timestamp = config.session_timestamp;
            opts.config_yaml = config_yaml;
            opts.channel_mask = config.channel_mask;
            opts.split_index = split_index;
            opts.chunk_frames = config.chunk_frames;
            opts.compression_level = config.compression_level;
            opts.swmr = config.swmr;
            opts.remission = config.remission;
            opts.device_firmware = config.device_firmware;
            opts.device_order_number = config.device_order_number;
            opts.device_type = config.device_type;
            opts.device_name = config.device_name;
            opts.device_keeps_flagged_points = config.device_keeps_flagged_points;
            opts.audit = config.audit;
            opts.start_angle_deg = ScanFixed::kStartAngleDeg;
            opts.stop_angle_deg = ScanFixed::kStopAngleDeg;
            opts.angle_step_deg = ScanFixed::kAngularResolutionDeg;
            opts.output_rate = ScanFixed::kOutputRate;

            if (!file.Open(opts)) {
                g_log.Error("[{}] [Writer] Cannot create '{}': {}", config.instance, opts.path, file.LastError());
                return false;
            }
            file_bytes = 0;
            stat_files_created.fetch_add(1, std::memory_order_relaxed);

            if (config.compression_level > 0 && !file.CompressionActive() && !compression_warned) {
                compression_warned = true;
                g_log.Warn("[{}] [Writer] gzip level {} requested but the deflate filter is unavailable in this "
                           "libhdf5 build — recording uncompressed", config.instance, config.compression_level);
            }
            g_log.Trace("[{}] [Writer] Opened split file {}", config.instance, opts.path);
            return true;
        }

        // Rolls over to the next split file at the payload threshold
        [[nodiscard]] bool WriteBatch(const ScanRecord *records, std::size_t count) {
            if (!file.Append(records, count)) {
                g_log.Error("[{}] [Writer] HDF5 append failed on '{}': {}", config.instance, SplitPath(split_index),
                            file.LastError());
                // Popped but not written: keep written + dropped == parsed
                stat_frames_dropped.fetch_add(count, std::memory_order_relaxed);
                return false;
            }
            const std::uint64_t bytes = static_cast<std::uint64_t>(count) * record_bytes;
            file_bytes += bytes;
            stat_bytes_written.fetch_add(static_cast<std::size_t>(bytes), std::memory_order_relaxed);
            stat_frames_written.fetch_add(count, std::memory_order_relaxed);

            if (config.max_file_bytes > 0 && file_bytes >= config.max_file_bytes) {
                std::lock_guard<std::mutex> rollover(file_mutex); // OnTelemetry must not see the gap
                if (!file.Close()) {
                    g_log.Error("[{}] [Writer] Could not finish '{}': {} — the split may be truncated",
                                config.instance, SplitPath(split_index), file.LastError());
                    return false;
                }
                ++split_index;
                if (!OpenSplit()) {
                    return false;
                }
            }
            return true;
        }

        // Full batches are written immediately, partial ones at each flush tick.
        // Stops on the first failure (HasFailed()); later scans count as dropped
        void WriteLoop() {
            g_log.Trace("[{}] [Writer] Write thread started", config.instance);

            std::vector<ScanRecord> batch(config.chunk_frames);
            std::size_t count = 0;
            const auto flush_interval = std::chrono::milliseconds(config.flush_interval_ms);
            auto last_flush = std::chrono::steady_clock::now();

            while (running.load(std::memory_order_acquire)) {
                std::size_t popped = 0;
                while (count < batch.size() && queue->try_pop(batch[count])) {
                    ++count;
                    ++popped;
                }

                if (count == batch.size()) {
                    if (!WriteBatch(batch.data(), count)) {
                        failed.store(true, std::memory_order_release);
                        g_log.Error("[{}] [Writer] Stopping the write loop — further scans are dropped",
                                    config.instance);
                        return;
                    }
                    count = 0;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now - last_flush >= flush_interval) {
                    if (count > 0) {
                        if (!WriteBatch(batch.data(), count)) {
                            failed.store(true, std::memory_order_release);
                            g_log.Error("[{}] [Writer] Stopping the write loop — further scans are dropped",
                                        config.instance);
                            return;
                        }
                        count = 0;
                    }
                    if (!file.Flush()) {
                        failed.store(true, std::memory_order_release);
                        g_log.Error("[{}] [Writer] {} — stopping the write loop, further scans are dropped",
                                    config.instance, file.LastError());
                        return;
                    }
                    last_flush = now;
                }

                if (popped == 0) {
                    std::this_thread::sleep_for(kWriteBackoffSleep);
                }
            }

            // Drain
            while (true) {
                while (count < batch.size() && queue->try_pop(batch[count])) {
                    ++count;
                }
                if (count == 0) {
                    break;
                }
                if (!WriteBatch(batch.data(), count)) {
                    failed.store(true, std::memory_order_release);
                    g_log.Error("[{}] [Writer] HDF5 append failed during the final drain", config.instance);
                    return;
                }
                count = 0;
            }

            g_log.Trace("[{}] [Writer] Write thread stopped", config.instance);
        }

        // Validate every scan against the fixed file contract. Exact float
        // equality is intentional: accepted device values equal the stored
        // attributes; deviations (including NaN) are never rounded away.
        bool CheckChannelScaling(const ScanData &scan) {
            for (const auto &spec: kChannelSpecs) {
                if (!(config.channel_mask & spec.bit)) {
                    continue;
                }
                float factor = spec.scale_factor;
                float offset = spec.scale_offset;
                bool present = false;
                if (spec.bit == ChannelMask::kQlty1) {
                    if (const auto *ch = scan.QualityChannel()) {
                        factor = ch->scale_factor;
                        offset = ch->scale_offset;
                        present = true;
                    }
                } else {
                    const ChannelData16 *ch = nullptr;
                    switch (spec.bit) {
                        case ChannelMask::kDist1:
                            ch = scan.DistanceChannel();
                            break;
                        case ChannelMask::kRssi1:
                            ch = scan.RssiChannel();
                            break;
                        case ChannelMask::kRefl1:
                            ch = scan.ReflectanceChannel();
                            break;
                        case ChannelMask::kAngl1:
                            ch = scan.AngleCorrectionChannel();
                            break;
                        default:
                            break;
                    }
                    if (ch != nullptr) {
                        factor = ch->scale_factor;
                        offset = ch->scale_offset;
                        present = true;
                    }
                }
                if (!present) {
                    g_log.Error("[{}] [Writer] Required channel {} missing", config.instance, spec.content);
                    return false;
                }
                if (factor != spec.scale_factor || offset != spec.scale_offset) {
                    g_log.Error("[{}] [Writer] Channel {} reports scale ({}, {}) but the file requires "
                               "({}, {}) — rejecting scan; recording is INCOMPLETE",
                               config.instance, spec.content, factor, offset, spec.scale_factor, spec.scale_offset);
                    return false;
                }
            }
            return true;
        }
    };


    ScanRecordWriter::ScanRecordWriter(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {
    }


    ScanRecordWriter::~ScanRecordWriter() {
        if (impl_->running.load(std::memory_order_relaxed)) {
            Stop();
        }
    }


    bool ScanRecordWriter::Start() {
        if (impl_->running.load(std::memory_order_relaxed)) {
            g_log.Warn("[{}] [Writer] Already running", impl_->config.instance);
            return false;
        }
        if (impl_->config.chunk_frames == 0 || impl_->config.flush_interval_ms == 0 ||
            impl_->config.compression_level < 0 || impl_->config.compression_level > 9) {
            g_log.Error("[{}] [Writer] Invalid recording configuration (chunk_frames {}, flush_interval_ms {}, "
                        "compression_level {})", impl_->config.instance, impl_->config.chunk_frames,
                        impl_->config.flush_interval_ms, impl_->config.compression_level);
            return false;
        }

        impl_->record_bytes = RecordBytes(impl_->config.channel_mask);
        impl_->config_yaml = ReadTextFile(impl_->config.config_yaml_path, kMaxConfigYamlBytes);
        if (!impl_->config.config_yaml_path.empty() && impl_->config_yaml.empty()) {
            g_log.Trace("[{}] [Writer] Config snapshot '{}' not embedded (unreadable or > {} KB)",
                        impl_->config.instance, impl_->config.config_yaml_path, kMaxConfigYamlBytes / 1024);
        }

        const std::filesystem::path parent = std::filesystem::path(impl_->config.output_path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        // Eager so a bad path fails Start()
        impl_->split_index = 0;
        impl_->compression_warned = false;
        impl_->failed.store(false, std::memory_order_relaxed);
        if (!impl_->OpenSplit()) {
            return false;
        }
        // Before the write thread owns `file`
        const bool gzip_active = impl_->file.CompressionActive();

        impl_->queue = std::make_unique<common::RingBuffer<ScanRecord> >(impl_->config.queue_capacity);

        impl_->running.store(true, std::memory_order_release);
        impl_->write_thread = std::thread([this]() { impl_->WriteLoop(); });

        g_log.Info("[{}] [Writer] Recording to {} (record: {} B, channels: 0x{:02X} [{}], chunk: {} scans, "
                   "flush: {} ms, gzip: {}, swmr: {}, queue: {} frames, max file: {} MB)",
                   impl_->config.instance, impl_->config.output_path, impl_->record_bytes,
                   impl_->config.channel_mask, ChannelNames(impl_->config.channel_mask), impl_->config.chunk_frames,
                   impl_->config.flush_interval_ms,
                   gzip_active ? std::to_string(impl_->config.compression_level) : "off",
                   impl_->config.swmr ? "on" : "off", impl_->config.queue_capacity,
                   impl_->config.max_file_bytes / (1024 * 1024));
        return true;
    }


    void ScanRecordWriter::Stop() {
        if (!impl_->running.load(std::memory_order_relaxed)) {
            return;
        }

        impl_->running.store(false, std::memory_order_release);

        if (impl_->write_thread.joinable()) {
            impl_->write_thread.join();
        }

        // Still queued = the loop stopped on an error
        if (impl_->queue) {
            const std::size_t left = impl_->queue->size();
            if (left > 0) {
                impl_->stat_frames_dropped.fetch_add(left, std::memory_order_relaxed);
                g_log.Warn("[{}] [Writer] {} queued scans discarded at stop", impl_->config.instance, left);
            }
        }

        // H5Fclose performs the final metadata flush: a failure means the last split is truncated
        if (!impl_->file.Close()) {
            impl_->failed.store(true, std::memory_order_release);
            g_log.Error("[{}] [Writer] Could not finish '{}': {} — that file is INCOMPLETE",
                        impl_->config.instance, impl_->SplitPath(impl_->split_index), impl_->file.LastError());
        }
        impl_->queue.reset();

        g_log.Trace("[{}] [Writer] Recording stopped", impl_->config.instance);
    }


    void ScanRecordWriter::OnScan(const ScanData &scan) {
        if (!impl_->queue) {
            impl_->stat_frames_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (impl_->failed.load(std::memory_order_acquire) || !impl_->CheckChannelScaling(scan)) {
            impl_->failed.store(true, std::memory_order_release);
            impl_->stat_frames_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        ScanRecord record{};
        FillScanRecord(scan, impl_->config.channel_mask, record);

        if (impl_->queue->try_push(std::move(record))) {
            impl_->stat_frames_queued.fetch_add(1, std::memory_order_relaxed);
        } else {
            impl_->stat_frames_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }


    void ScanRecordWriter::OnTelemetry(const TelemetrySample &sample) {
        // Owner thread, seconds apart. Straight into the file rather than
        // through the scan queue: it must not compete with 600 Hz of scans for
        // queue slots, and losing a telemetry row to a full queue would be a
        // silent hole in the health record.
        if (!impl_->running.load(std::memory_order_acquire) || impl_->failed.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> rollover(impl_->file_mutex);
        if (!impl_->file.AppendTelemetry(sample)) {
            impl_->failed.store(true, std::memory_order_release);
            g_log.Error("[{}] [Writer] Telemetry row not written: {}", impl_->config.instance,
                        impl_->file.LastError());
        }
    }


    ScanRecordWriter::Statistics ScanRecordWriter::GetStatistics() const {
        return {
            impl_->stat_frames_written.load(std::memory_order_relaxed),
            impl_->stat_frames_dropped.load(std::memory_order_relaxed),
            impl_->stat_bytes_written.load(std::memory_order_relaxed),
            impl_->stat_files_created.load(std::memory_order_relaxed),
            impl_->stat_frames_queued.load(std::memory_order_relaxed),
        };
    }


    bool ScanRecordWriter::HasFailed() const {
        return impl_->failed.load(std::memory_order_acquire);
    }
} // namespace lms4xxx
