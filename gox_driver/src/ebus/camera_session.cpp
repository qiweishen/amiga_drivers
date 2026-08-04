#include "ebus/camera_session.hpp"

#include <cstring>

#include "logger.h"
#include "ebus/sdk_error.hpp"

namespace jai::ebus {
    namespace {
        Common::DriverLog g_log{"GoX"};
    } // namespace

    CameraSession::CameraSession(uint32_t camera_index, const CameraConfig &cfg, const AppConfig &app,
                                 const uint8_t session_uuid[16],
                                 StopController *stop) : camera_index_(camera_index), cfg_(cfg), app_(app),
                                                         stop_(stop), reporter_(cfg.id, &stats_) {
        std::memcpy(session_uuid_, session_uuid, sizeof(session_uuid_));
    }

    CameraSession::~CameraSession() {
        try {
            stop_and_join();
        } catch (const std::exception &e) {
            g_log.error("[{}] teardown in destructor failed: {}", cfg_.id, e.what());
        }
    }

    void CameraSession::start(const std::string &session_dir) {
        camera_dir_ = session_dir + "/" + cfg_.id;
        bring_up_session_();
        start_streaming_();
    }

    void CameraSession::bring_up_session_() {
        // Connect
        try {
            controller_ = std::make_unique<CameraController>(cfg_.id, stop_);
            if (!cfg_.device.mac.empty()) {
                DiscoveredDevice dev = find_camera(cfg_.device.mac, cfg_.device.force_ip);
                const std::string target = dev.connection_id;
                controller_->connect(target, std::move(dev));
            } else {
                DiscoveredDevice identity;
                identity.ip = cfg_.device.ip;
                controller_->connect(cfg_.device.ip, std::move(identity));
            }
        } catch (const std::exception &e) {
            throw std::runtime_error("[" + cfg_.id + "] Connect: " + e.what());
        }

        // PTP: enable first so BMCA/servo convergence overlaps the rest of
        // the bring-up as little as the task ordering allows, then wait
        ptp_ = std::make_unique<PtpManager>(cfg_.id, controller_->params(), app_.ptp);
        if (!ptp_->enable()) {
            throw std::runtime_error("[" + cfg_.id + "] PTP enable failed (on_timeout=abort)");
        }
        if (!ptp_->wait_for_sync(stop_)) {
            throw std::runtime_error("[" + cfg_.id + "] PTP synchronization not reached (on_timeout=abort)");
        }

        // Stream open (rx buffer, negotiate, destination, SCPD, tuning).
        receiver_ = std::make_unique<StreamReceiver>(camera_index_, cfg_, app_.output, controller_.get(), stop_, &stats_);
        try {
            receiver_->open();
        } catch (const std::exception &e) {
            throw std::runtime_error("[" + cfg_.id + "] Stream open: " + e.what());
        }

        // GenICam configuration (ordered apply engine + verification).
        try {
            controller_->apply_config(cfg_);
        } catch (const std::exception &e) {
            throw std::runtime_error("[" + cfg_.id + "] GenICam apply: " + e.what());
        }

        // First PTP offset record (session-start baseline for drift reporting).
        ptp_->refresh_offset();
    }

    void CameraSession::start_streaming_() {
        // --- Buffers, recorder, queue/pool.
        try {
            receiver_->allocate_buffers();
        } catch (const std::exception &e) {
            throw std::runtime_error("[" + cfg_.id + "] Buffer allocation: " + e.what());
        }

        RecorderOptions opts;
        opts.camera_dir = camera_dir_;
        opts.camera_id = cfg_.id;
        opts.camera_serial = controller_->identity().serial;
        std::memcpy(opts.session_uuid, session_uuid_, sizeof(opts.session_uuid));
        opts.segment_max_bytes = static_cast<uint64_t>(
            app_.output.segment_size_gib * static_cast<double>(1ull << 30));
        opts.record_align = app_.output.record_align;
        opts.flush_interval_bytes = static_cast<uint64_t>(app_.output.flush_interval_mb) << 20;
        opts.min_free_bytes = static_cast<uint64_t>(app_.disk.min_free_gb * static_cast<double>(1ull << 30));

        // Pool = queue capacity + 2 (one chunk in the writer, one in the acquirer);
        // with fewer the pool runs dry first and "block" degrades to drop_newest.
        try {
            pool_ = std::make_unique<ChunkPool>(app_.output.queue_max_frames + 2,
                                                receiver_->expected_payload_size());
            queue_ = std::make_unique<BoundedQueue<FrameChunkPtr> >(app_.output.queue_max_frames);
        } catch (const std::bad_alloc &) {
            throw std::runtime_error("[" + cfg_.id + "] cannot allocate " +
                                                     std::to_string(app_.output.queue_max_frames + 2) +
                                                     " frame chunks of " + std::to_string(
                                                         receiver_->expected_payload_size()) +
                                                     " bytes each (out of memory; lower recording.queue_max_frames)");
        }
        stats_.queue_capacity.store(queue_->capacity(), std::memory_order_relaxed);
        recorder_ = std::make_unique<Recorder>(opts, &stats_);
        try {
            recorder_->open();
        } catch (const IoError &e) {
            throw std::runtime_error("[" + cfg_.id + "] Recorder open: " + e.what());
        }

        // --- Go: TLParamsLocked, then AcquisitionStart, then the worker threads.
        try {
            controller_->stream_enable();
            controller_->acquisition_start();
        } catch (const std::exception &e) {
            throw std::runtime_error("[" + cfg_.id + "] AcquisitionStart: " + e.what());
        }
        started_ = true;
        acq_thread_ = std::thread(&CameraSession::acq_thread_main, this);
        writer_thread_ = std::thread(&CameraSession::writer_thread_main, this);
        g_log.info("[{}] recording to {}", cfg_.id, camera_dir_);
    }

    void CameraSession::acq_thread_main() {
        try {
            const double no_frame_abort_s =
                    app_.watchdog.resolved_no_frame_abort_s(cfg_.acquisition.trigger.mode);
            receiver_->run_acquisition(*pool_, *queue_, app_.output.max_frames, app_.watchdog, no_frame_abort_s);
        } catch (const std::exception &e) {
            g_log.error("[{}] acquisition thread failed: {}", cfg_.id, e.what());
            stop_->request_stop(StopReason::Error);
        }
        // EOS for the writer: close() lets pop() drain the remaining items.
        queue_->close();
    }

    void CameraSession::writer_thread_main() {
        bool io_failed = false;
        FrameChunkPtr chunk;
        while (queue_->pop(chunk)) {
            stats_.queue_depth.store(queue_->size(), std::memory_order_relaxed);
            if (!io_failed) {
                try {
                    recorder_->write_frame(chunk->meta, chunk->data.get(),
                                           static_cast<size_t>(chunk->meta.payload_size));
                } catch (const IoError &e) {
                    g_log.error("[{}] write failed: {}", cfg_.id, e.what());
                    stop_->request_stop(StopReason::Error);
                    io_failed = true; // keep draining to unblock the producer
                }
            }
            pool_->release(std::move(chunk));
        }
        try {
            recorder_->close();
        } catch (const IoError &e) {
            g_log.error("[{}] recorder close failed: {}", cfg_.id, e.what());
            stop_->request_stop(StopReason::Error);
        }
    }

    void CameraSession::teardown_devices() {
        if (receiver_) {
            try {
                controller_->acquisition_stop(/*ignore_errors=*/true);
            } catch (const std::exception &e) {
                g_log.debug("[{}] acquisition stop during teardown: {}", cfg_.id, e.what());
            }
            receiver_->teardown();
        }
        if (controller_) {
            controller_->disconnect();
        }
    }

    void CameraSession::stop_and_join() {
        if (stopped_) {
            return;
        }
        stopped_ = true;

        if (!started_) {
            // Failed bring-up: devices only, no threads.
            teardown_devices();
            return;
        }

        // Ordered shutdown — the sequence is a constraint:
        // 0. force the acquisition loop to exit (a teardown must never hang on join)
        receiver_->request_stop_local();

        // 1. stop the source (errors ignored when the link is already gone)
        try {
            controller_->acquisition_stop(/*ignore_errors=*/true);
        } catch (const std::exception &e) {
            g_log.debug("[{}] AcquisitionStop during shutdown: {}", cfg_.id, e.what());
        }

        // 2. acquisition thread drains RetrieveBuffer, closes the queue, exits
        if (acq_thread_.joinable()) {
            acq_thread_.join();
        }

        // 3. final stream statistics (needs the open stream), then stream teardown
        receiver_->poll_stream_stats();
        receiver_->dump_stream_params(camera_dir_ + "/stream_stats.txt");
        receiver_->teardown();

        // 4. writer drains the closed queue and closes the recorder
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }

        // 5. control channel down last
        controller_->disconnect();
        g_log.info("[{}] session stopped", cfg_.id);
    }

    bool CameraSession::clean() const {
        const CameraStats::Snapshot s = stats_.snapshot();
        return s.frames_incomplete == 0 && s.frames_error_dropped == 0 && s.frames_dropped_queue == 0 && s.
               frames_lost_gap == 0 &&
               s.stream_blocks_dropped == 0;
    }

    void CameraSession::poll_stream_stats() {
        if (started_ && !stopped_ && receiver_) {
            receiver_->poll_stream_stats();
        }
    }

    void CameraSession::refresh_ptp_offset() {
        if (started_ && !stopped_ && ptp_ && controller_ && controller_->connected()) {
            ptp_->refresh_offset();
        }
    }
} // namespace jai::ebus
