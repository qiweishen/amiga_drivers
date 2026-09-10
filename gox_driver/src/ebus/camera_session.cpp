#include "ebus/camera_session.h"
#include "time_util.h"

#include <algorithm>
#include <cstring>

#include "buffering.h"
#include "logger.h"
#include "util.h"
#include "ebus/sdk_error.h"

namespace gox::ebus {
    namespace {
        common::DriverLog g_log{"GoX"};
    } // namespace

    CameraSession::CameraSession(const CameraConfig &cfg, const AppConfig &app, const uint8_t session_uuid[16],
                                 StopController *stop) : cfg_(cfg), app_(app), stop_(stop),
                                                         reporter_(cfg.id, &stats_) {
        std::memcpy(session_uuid_, session_uuid, sizeof(session_uuid_));
    }

    CameraSession::~CameraSession() {
        try {
            StopAndJoin();
        } catch (const std::exception &e) {
            g_log.Error("[{}] [eBUS] teardown in destructor failed: {}", cfg_.id, e.what());
        }
    }

    void CameraSession::Start(const std::string &session_dir) {
        camera_dir_ = session_dir + "/" + cfg_.id;
        BringUpSession();
        StartStreaming();
    }

    void CameraSession::BringUpSession() {
        // Connect
        try {
            controller_ = std::make_unique<CameraController>(cfg_.id, stop_);
            if (!cfg_.device.mac.empty()) {
                DiscoveredDevice dev = common::Ebus::FindCamera(cfg_.device.mac);
                const std::string target = dev.connection_id;
                controller_->Connect(target, std::move(dev));
            } else {
                DiscoveredDevice identity;
                identity.ip = cfg_.device.ip;
                controller_->Connect(cfg_.device.ip, std::move(identity));
            }
        } catch (const std::exception &e) {
            throw std::runtime_error("Connect: " + std::string(e.what()));
        }

        // Factory baseline: previous users' settings must not reach the
        // recording. Before PTP (the load would reset GevIEEE1588) and before
        // the stream Open (it resets GevSCPSPacketSize/GevSCPD).
        try {
            factory_load_ = controller_->LoadFactoryDefaults();
        } catch (const std::exception &e) {
            throw std::runtime_error("Factory defaults: " + std::string(e.what()));
        }

        // PTP: enable first so BMCA/servo convergence overlaps the rest of
        // the bring-up as little as the task ordering allows, then wait
        ptp_ = std::make_unique<PtpManager>(cfg_.id, controller_->Params(), app_.ptp);
        if (!ptp_->Enable()) {
            throw std::runtime_error("PTP enable failed (on_timeout=abort)");
        }
        if (!ptp_->WaitForSync(stop_)) {
            throw std::runtime_error("PTP synchronization not reached (on_timeout=abort)");
        }

        // Stream Open (rx buffer, negotiate, destination, receiver tuning).
        receiver_ = std::make_unique<StreamReceiver>(cfg_, app_.output, controller_.get(), stop_, &stats_);
        try {
            receiver_->Open();
        } catch (const std::exception &e) {
            throw std::runtime_error("Stream open: " + std::string(e.what()));
        }

        // GenICam configuration (build_apply_plan order + read-back verification).
        try {
            applied_ = controller_->ApplyConfig(cfg_);
            counter_bound_ = Counter0Bound(applied_);
        } catch (const std::exception &e) {
            throw std::runtime_error("GenICam apply: " + std::string(e.what()));
        }

    }

    void CameraSession::StartStreaming() {
        // --- Buffers, recorder, queue/pool.
        try {
            receiver_->AllocateBuffers();
        } catch (const std::exception &e) {
            throw std::runtime_error("Buffer allocation: " + std::string(e.what()));
        }

        RecorderOptions opts;
        opts.camera_dir = camera_dir_;
        opts.camera_id = cfg_.id;
        opts.camera_serial = controller_->Identity().serial;
        std::memcpy(opts.session_uuid, session_uuid_, sizeof(opts.session_uuid));
        opts.segment_max_bytes = static_cast<uint64_t>(app_.output.segment_size_mb) << 20;
        opts.record_align = app_.output.record_align;
        opts.flush_interval_bytes = static_cast<uint64_t>(app_.output.flush_interval_mb) << 20;

        // Pool = queue capacity + 2 (one chunk in the writer, one in the acquirer);
        // with fewer the pool runs dry first and "block" degrades to drop_newest.
        // A GO-X frame is ~18.6 MB, so the count is real memory: 0 = auto sizes
        // it from the frame rate instead of holding ten seconds of video.
        const uint32_t queue_frames =
                app_.output.queue_max_frames != 0
                    ? app_.output.queue_max_frames
                    : AutoQueueFrames(cfg_.acquisition.frame_rate_hz ? *cfg_.acquisition.frame_rate_hz : 0.0);
        try {
            pool_ = std::make_unique<ChunkPool>(queue_frames + 2, receiver_->ExpectedPayloadSize());
            queue_ = std::make_unique<common::BoundedQueue<FrameChunkPtr> >(queue_frames);
        } catch (const std::bad_alloc &) {
            throw std::runtime_error("cannot allocate " + std::to_string(queue_frames + 2) +
                                     " frame chunks of " + std::to_string(receiver_->ExpectedPayloadSize()) +
                                     " bytes each (out of memory; lower output.queue_max_frames)");
        }
        stats_.queue_capacity.store(queue_->capacity(), std::memory_order_relaxed);
        recorder_ = std::make_unique<Recorder>(opts, &stats_);
        try {
            recorder_->Open();
        } catch (const IoError &e) {
            throw std::runtime_error("Recorder open: " + std::string(e.what()));
        }

        // --- Session metadata. <cam>/ exists only now (Recorder::Open created
        // it), and the camera is still idle, so these reads cost nothing.
        runtime_shape_.buffer_count = static_cast<uint32_t>(receiver_->BufferCount());
        runtime_shape_.queue_frames = queue_frames;
        runtime_shape_.socket_rx_requested_bytes = receiver_->SocketRxRequestedBytes();
        runtime_shape_.socket_rx_effective_bytes = receiver_->SocketRxEffectiveBytes();
        runtime_shape_.socket_rx_set_result = receiver_->SocketRxSetResult();
        runtime_shape_.socket_rx_read_result = receiver_->SocketRxReadResult();
        runtime_shape_.packet_size = receiver_->PacketSize();
        runtime_shape_.payload_size = receiver_->ExpectedPayloadSize();
        WriteDeviceJson();
        telemetry_.open(camera_dir_ + "/telemetry.jsonl", std::ios::out | std::ios::app);
        if (!telemetry_) {
            throw std::runtime_error("cannot open required telemetry.jsonl in " + camera_dir_);
        }

        // --- Go: TLParamsLocked, then AcquisitionStart, then the worker threads.
        try {
            controller_->StreamEnable();
            // Counter0 must start from zero at AcquisitionStart: the apply plan
            // armed it earlier, and any trigger that arrived between the apply
            // and here would be counted without ever producing a frame
            // (CounterEventSource=Off stops a counter but does not clear it,
            // manual p.116).
            if (counter_bound_ && !controller_->ResetTriggerCounter()) {
                g_log.Warn("[{}] [eBUS] CounterReset failed; the missed-trigger count starts from an "
                           "unknown baseline", cfg_.id);
            }
            controller_->AcquisitionStart();
        } catch (const std::exception &e) {
            throw std::runtime_error("AcquisitionStart: " + std::string(e.what()));
        }
        started_ = true;
        PollDeviceTelemetry(); // first row: the Counter0 and temperature baseline
        acq_thread_ = std::thread(&CameraSession::AcqThreadMain, this);
        writer_thread_ = std::thread(&CameraSession::WriterThreadMain, this);
        g_log.Info("[{}] [eBUS] recording to {}", cfg_.id, camera_dir_);
    }

    void CameraSession::AcqThreadMain() {
        try {
            receiver_->RunAcquisition(*pool_, *queue_, app_.output.max_frames);
        } catch (const std::exception &e) {
            g_log.Error("[{}] [eBUS] acquisition thread failed: {}", cfg_.id, e.what());
            stop_->RequestStop(StopReason::kError);
        }
        // Whatever ended the loop, this camera is no longer acquiring: disarm
        // Main's no-data watchdog rather than let it read a frozen reference.
        stats_.data_reference_mono_ns.store(0, std::memory_order_release);
        // EOS for the writer: close() lets pop() drain the remaining items.
        queue_->close();
    }

    void CameraSession::WriterThreadMain() {
        bool io_failed = false;
        FrameChunkPtr chunk;
        while (queue_->pop(chunk)) {
            stats_.queue_depth.store(queue_->size(), std::memory_order_relaxed);
            if (!io_failed) {
                try {
                    recorder_->WriteFrame(chunk->meta, chunk->data.get(),
                                           static_cast<size_t>(chunk->meta.payload_size));
                } catch (const IoError &e) {
                    g_log.Error("[{}] [eBUS] write failed: {}", cfg_.id, e.what());
                    stop_->RequestStop(StopReason::kError);
                    io_failed = true; // keep draining to unblock the producer
                }
            }
            pool_->Release(std::move(chunk));
        }
        try {
            recorder_->close(!io_failed);
        } catch (const IoError &e) {
            g_log.Error("[{}] [eBUS] recorder close failed: {}", cfg_.id, e.what());
            stop_->RequestStop(StopReason::kError);
        }
    }

    void CameraSession::TeardownDevices() {
        if (receiver_) {
            try {
                controller_->AcquisitionStop(/*ignore_errors=*/true);
            } catch (const std::exception &e) {
                g_log.Debug("[{}] [eBUS] acquisition stop during teardown: {}", cfg_.id, e.what());
            }
            receiver_->Teardown();
        }
        if (controller_) {
            controller_->Disconnect();
        }
    }

    void CameraSession::StopAndJoin() {
        if (stopped_) {
            return;
        }
        stopped_ = true;

        if (!started_) {
            // Failed bring-up: devices only, no threads.
            TeardownDevices();
            return;
        }

        // Ordered shutdown — the sequence is a constraint:
        // 0. force the acquisition loop to exit (a teardown must never hang on join)
        receiver_->RequestStopLocal();

        // 1. stop the source (errors ignored when the link is already gone)
        try {
            controller_->AcquisitionStop(/*ignore_errors=*/true);
        } catch (const std::exception &e) {
            g_log.Debug("[{}] [eBUS] AcquisitionStop during shutdown: {}", cfg_.id, e.what());
        }

        // 2. acquisition thread drains RetrieveBuffer, closes the queue, exits
        if (acq_thread_.joinable()) {
            acq_thread_.join();
        }

        // 3. final stream statistics (needs the open stream), then stream teardown
        receiver_->PollStreamStats();
        receiver_->DumpStreamParams(camera_dir_ + "/stream_stats.txt");
        // Last telemetry row while the control channel is still up: the final
        // Counter0 value is what the Final line's missed= is computed against.
        PollDeviceTelemetry();
        if (telemetry_.is_open()) {
            telemetry_.close();
            if (!telemetry_) {
                telemetry_failed_ = true;
                g_log.Error("[{}] [eBUS] telemetry.jsonl close failed", cfg_.id);
                stop_->RequestStop(StopReason::kError);
            }
        }
        receiver_->Teardown();

        // 4. writer drains the closed queue and closes the recorder
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }

        // 5. control channel down last
        controller_->Disconnect();
        g_log.Info("[{}] [eBUS] session stopped", cfg_.id);
    }

    std::optional<uint64_t> CameraSession::MicrosSinceLastData() const {
        if (cfg_.acquisition.trigger.mode == TriggerMode::kExternal) {
            // Silence is the operator's doing (the pulses stopped), not a fault.
            return std::nullopt;
        }
        const uint64_t reference = stats_.data_reference_mono_ns.load(std::memory_order_acquire);
        if (reference == 0) {
            return std::nullopt; // not acquiring
        }
        const uint64_t now = common::TimeUtil::MonotonicNowNs();
        return now > reference ? (now - reference) / 1000ull : 0;
    }

    bool CameraSession::Clean() const {
        const CameraStats::Snapshot s = stats_.GetSnapshot();
        return !telemetry_failed_ && s.frames_incomplete == 0 && s.frames_error_dropped == 0 && s.frames_dropped_queue == 0 && s.
               frames_lost_gap == 0 &&
               s.stream_blocks_dropped == 0;
    }

    void CameraSession::PollStreamStats() {
        if (started_ && !stopped_ && receiver_) {
            receiver_->PollStreamStats();
        }
    }

    std::string CameraSession::FirstLossDescription() const {
        const CameraStats::Snapshot s = stats_.GetSnapshot();
        if (s.frames_dropped_queue != 0) return "frame queue overflow (" + std::to_string(s.frames_dropped_queue) + " dropped)";
        if (s.frames_lost_gap != 0) return "BlockID gap (" + std::to_string(s.frames_lost_gap) + " frames missing)";
        if (s.frames_incomplete != 0) return "incomplete frame (" + std::to_string(s.frames_incomplete) + ")";
        if (s.frames_error_dropped != 0) return "buffer error (" + std::to_string(s.frames_error_dropped) + " dropped)";
        if (s.stream_blocks_dropped != 0) return "stream-layer block drop (" + std::to_string(s.stream_blocks_dropped) + ")";
        return {};
    }

    void CameraSession::WriteDeviceJson() {
        // Version-1 raw headers do not contain the full interpretation contract.
        // Persist it before StreamEnable/AcquisitionStart; failures abort bring-up.
        try {
            const DeviceReport report = controller_->CollectDeviceReport(
                cfg_, applied_, ptp_ ? ptp_->Summary() : PtpSummary{}, factory_load_, runtime_shape_);
            const std::string path = camera_dir_ + "/device.json";
            PublishMetadata(path, BuildDeviceJson(report).dump(2) + "\n");
            g_log.Info("[{}] [eBUS] device metadata written to {}", cfg_.id, path);
        } catch (const std::exception &e) {
            throw std::runtime_error("required device.json: " + std::string(e.what()));
        }
    }

    void CameraSession::PollDeviceTelemetry() {
        // Deliberately not gated on !stopped_ (unlike refresh_ptp_offset): the
        // last row is written from stop_and_join, while the control channel is
        // still up. Connected() is what actually decides.
        if (!started_ || !controller_ || !controller_->Connected()) {
            return;
        }
        try {
            TelemetrySample sample = controller_->SampleTelemetry(counter_bound_);
            // PTP values come from the guard that ran on this same tick, not
            // from a second round of GVCP reads.
            if (ptp_) {
                const PtpSummary ptp = ptp_->Summary();
                if (ptp.enabled) {
                    sample.ptp_status = ptp.status;
                    if (ptp.accuracy >= 0) {
                        sample.ptp_accuracy = ptp.accuracy;
                    }
                }
            }

            if (sample.temp_sensor) {
                stats_.sensor_temp_centi.store(static_cast<int32_t>(*sample.temp_sensor * 100.0),
                                               std::memory_order_relaxed);
            }
            if (sample.trig) {
                stats_.trigger_count.store(*sample.trig, std::memory_order_relaxed);
                stats_.trigger_overflow.store(sample.trig_overflow, std::memory_order_relaxed);
            }

            // Manual p.173: the camera's internal temperature must stay below
            // 72 C. Warn once per excursion (re-armed below 67 C) and keep
            // recording - stopping is the operator's call, the evidence is in
            // telemetry.jsonl either way.
            const double hottest = std::max({
                sample.temp_sensor.value_or(0.0), sample.temp_mainboard.value_or(0.0),
                sample.temp_fpga.value_or(0.0)
            });
            if (!over_temperature_ && hottest >= kThermalLimitC) {
                over_temperature_ = true;
                g_log.Warn("[{}] [eBUS] camera temperature {:.1f} C is at or above the documented limit of "
                           "{:.0f} C (manual p.173); dark current and the hot-pixel population rise with it",
                           cfg_.id, hottest, kThermalLimitC);
            } else if (over_temperature_ && hottest <= kThermalRearmC) {
                over_temperature_ = false;
            }

            if (telemetry_.is_open() && !telemetry_failed_) {
                telemetry_ << BuildTelemetryLine(sample) << '\n';
                telemetry_.flush(); // userspace -> kernel; not a durability barrier
                if (!telemetry_) {
                    telemetry_failed_ = true;
                    g_log.Error("[{}] [eBUS] telemetry.jsonl write failed", cfg_.id);
                    stop_->RequestStop(StopReason::kError);
                }
            }
        } catch (const std::exception &e) {
            g_log.Warn("[{}] [eBUS] device telemetry poll failed: {}", cfg_.id, e.what());
        }
    }

    bool CameraSession::CheckPtpHealth() {
        if (!started_ || stopped_ || !ptp_ || !controller_ || !controller_->Connected()) {
            return true;
        }
        return ptp_->CheckHealth(stop_);
    }
} // namespace gox::ebus
