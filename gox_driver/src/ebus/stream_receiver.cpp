#include "ebus/stream_receiver.h"
#include "utility.h"
#include "time_util.h"

#include <PvImage.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

#include "buffering.h"
#include "format.h"
#include "logger.h"
#include "util.h"
#include "ebus/sdk_error.h"

namespace gox::ebus {
    namespace {
        common::DriverLog g_log{"GoX"};

        constexpr uint32_t kRetrieveTimeoutMs = 1000;
        constexpr uint32_t kDrainTimeoutMs = 500;
        // Upper bound on the post-stop drain. Without it, a camera that keeps
        // streaming (AcquisitionStop lost/failed on a congested control channel)
        // would feed the drain loop forever and shutdown would hang.
        constexpr uint64_t kDrainMaxTotalNs = 5ull * 1000000000ull;
        constexpr uint32_t kFallbackPacketSize = 1476; // safe on any 1500-MTU path
    } // namespace

    StreamReceiver::StreamReceiver(const CameraConfig &cfg, const OutputConfig &output,
                                   CameraController *controller, StopController *stop,
                                   CameraStats *stats) : camera_id_(cfg.id),
                                                         cfg_(cfg),
                                                         output_(output),
                                                         controller_(controller),
                                                         stop_(stop),
                                                         stats_(stats) {
    }

    StreamReceiver::~StreamReceiver() {
        try {
            Teardown();
        } catch (const std::exception &e) {
            g_log.Warn("[{}] [eBUS] stream teardown in destructor failed: {}", camera_id_, e.what());
        }
    }

    void StreamReceiver::Open() {
        const NetworkConfig &sc = cfg_.network;
        stream_ = std::make_unique<PvStreamGEV>();

        // eBUS 6.5.1 PvStreamGEV.h: set before Open(). On Linux, a request
        // above rmem_max uses that limit and returns INVALID_PARAMETER. The
        // post-Open readback may include doubled kernel bookkeeping space;
        // it is not directly comparable to the requested payload capacity.
        rx_buffer_requested_ = sc.socket_rx_buffer_mb * 1024u * 1024u;
        rx_buffer_effective_ = 0;
        rx_buffer_read_result_ = "NOT_QUERIED";
        PvResult r = stream_->SetUserModeSocketRxBufferSize(rx_buffer_requested_);
        rx_buffer_set_result_ = ToStd(r.GetCodeString());
        if (r.GetCode() == PvResult::Code::INVALID_PARAMETER) {
            g_log.Warn("[{}] [eBUS] SetUserModeSocketRxBufferSize({} bytes): {}. On Linux, eBUS 6.5.1 "
                       "uses net.core.rmem_max and returns INVALID_PARAMETER when the request exceeds that limit; "
                       "check sysctl net.core.rmem_max in the acquisition environment. Reading back after Open",
                       camera_id_, rx_buffer_requested_, PvResultToString(r));
        } else if (!r.IsOK()) {
            g_log.Warn("[{}] [eBUS] SetUserModeSocketRxBufferSize({} bytes) failed: {}; "
                       "requested capacity is not confirmed", camera_id_, rx_buffer_requested_,
                       PvResultToString(r));
        }

        // 2. Open. Port 0 = auto; local_ip pins the stream to one NIC.
        const std::string &device_ip = controller_->Identity().ip;
        // Channel 0: DeviceStreamChannelCount is 1 (Fixed) on the GO-X (manual p.125)
        CHECK_PV(stream_->Open(PvString(device_ip.c_str()), 0, 0, PvString(sc.local_ip.c_str())),
                 "PvStreamGEV::Open(" + device_ip + ")");
        local_ip_ = ToStd(stream_->GetLocalIPAddress());
        local_port_ = stream_->GetLocalPort();

        uint32_t effective_rx = 0;
        const PvResult rx_read = stream_->GetUserModeSocketRxBufferSize(effective_rx);
        rx_buffer_read_result_ = ToStd(rx_read.GetCodeString());
        if (rx_read.IsOK()) {
            rx_buffer_effective_ = effective_rx;
            g_log.Info("[{}] [eBUS] socket rx buffer: requested={} bytes, SDK SO_RCVBUF readback={} bytes, "
                       "set_result={}; Linux readback may include doubled bookkeeping allocation",
                       camera_id_, rx_buffer_requested_, rx_buffer_effective_, rx_buffer_set_result_);
            if (rx_buffer_effective_ < rx_buffer_requested_) {
                g_log.Warn("[{}] [eBUS] socket rx readback is below the requested size; "
                           "receive buffering is smaller than configured", camera_id_);
            }
        } else if (rx_read.GetCode() == PvResult::Code::NOT_SUPPORTED) {
            g_log.Info("[{}] [eBUS] socket rx readback: {}; this API only supports the user-mode receiver, "
                       "socket capacity is unavailable", camera_id_, PvResultToString(rx_read));
        } else {
            g_log.Warn("[{}] [eBUS] GetUserModeSocketRxBufferSize failed: {}; socket capacity is unknown",
                       camera_id_, PvResultToString(rx_read));
        }

        // 3. Packet size (device-side GevSCPSPacketSize, TLParamsLocked-guarded:
        // must happen before StreamEnable).
        PvDeviceGEV *dev = controller_->Device();
        if (sc.packet_size == 0) {
            r = dev->NegotiatePacketSize(0);
            if (!r.IsOK()) {
                g_log.Warn("[{}] [eBUS] NegotiatePacketSize failed ({}); falling back to SetPacketSize({})", camera_id_,
                           PvResultToString(r),
                           kFallbackPacketSize);
                CHECK_PV(dev->SetPacketSize(kFallbackPacketSize, 0), "PvDeviceGEV::SetPacketSize(fallback)");
            }
        } else {
            r = dev->SetPacketSize(sc.packet_size, 0);
            if (!r.IsOK()) {
                g_log.Warn("[{}] [eBUS] SetPacketSize({}) failed ({}); trying negotiation", camera_id_, sc.packet_size,
                           PvResultToString(r));
                r = dev->NegotiatePacketSize(0);
                if (!r.IsOK()) {
                    CHECK_PV(dev->SetPacketSize(kFallbackPacketSize, 0), "PvDeviceGEV::SetPacketSize(fallback)");
                }
            }
        }
        int64_t effective_ps = 0;
        if (ReadIntFeature(controller_->Params(), "GevSCPSPacketSize", effective_ps)) {
            packet_size_ = static_cast<uint32_t>(effective_ps);
        }
        g_log.Info("[{}] [eBUS] stream open: device {} -> {}:{}, packet size {}", camera_id_, device_ip,
                   local_ip_, local_port_, packet_size_);

        // 4. Point the device's stream channel at our receiver.
        CHECK_PV(dev->SetStreamDestination(stream_->GetLocalIPAddress(), stream_->GetLocalPort(), 0),
                 "PvDeviceGEV::SetStreamDestination");

        // 5. Receiver-side tuning escape hatch (PvStream GenICam parameters).
        // Device-side features are NOT written here: their legal ranges depend on
        // PixelFormat/ROI/GevGVSPExtendedIDMode, which apply_config writes next
        // (manual p.130), so every device write lives in the apply plan.
        for (const RawFeature &raw: sc.receiver_tuning) {
            FeatureWrite f;
            f.name = raw.name;
            f.value = raw.value;
            f.value_is_string = raw.value_is_string;
            f.strict = false; // escape hatch: the camera's clamping is its business
            apply_genicam_feature(stream_->GetParameters(), f, "[" + camera_id_ + "] stream");
        }
    }

    void StreamReceiver::AllocateBuffers() {
        if (!stream_ || !stream_->IsOpen()) {
            throw SdkError("allocate_buffers: stream not open");
        }
        expected_payload_size_ = controller_->PayloadSize();
        if (expected_payload_size_ == 0) {
            throw SdkError("device reports zero payload size");
        }

        // GetQueuedBufferMaximum() is the SDK's own ceiling: queueing past it
        // fails with an opaque error, so the auto rule clamps to it.
        const uint32_t queued_max = stream_->GetQueuedBufferMaximum();
        uint32_t count = cfg_.network.buffer_count;
        if (count == 0) {
            count = AutoBufferCount(cfg_.acquisition.frame_rate_hz ? *cfg_.acquisition.frame_rate_hz : 0.0,
                                      queued_max);
        } else if (queued_max != 0 && count > queued_max) {
            g_log.Warn("[{}] [eBUS] network.buffer_count {} exceeds the stream's maximum of {}; using {}",
                       camera_id_, count, queued_max, queued_max);
            count = queued_max;
        }

        buffers_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto buffer = std::make_unique<PvBuffer>();
            CHECK_PV(buffer->Alloc(static_cast<uint32_t>(expected_payload_size_)), "PvBuffer::Alloc");
            buffers_.push_back(std::move(buffer));
        }
        for (auto &buffer: buffers_) {
            // QueueBuffer reports PENDING on success: the buffer is queued and
            // awaiting data (no acquisition is running yet). Only treat other
            // non-OK codes as errors.
            const PvResult qr = stream_->QueueBuffer(buffer.get());
            if (!qr.IsOK() && qr.GetCode() != PvResult::Code::PENDING) {
                throw SdkError("PvStreamGEV::QueueBuffer", qr);
            }
        }
        g_log.Info("[{}] [eBUS] {} GVSP buffers of {} queued ({} total)", camera_id_, count,
                   common::HumanBytes(expected_payload_size_),
                   common::HumanBytes(count * expected_payload_size_));
    }

    bool StreamReceiver::process_buffer(PvBuffer *buffer, const PvResult &op_result, ChunkPool &pool,
                                        common::BoundedQueue<FrameChunkPtr> &queue, uint64_t max_frames) {
        // Clocks sampled immediately: these bracket the retrieve instant.
        const uint64_t host_rt = common::TimeUtil::RealtimeNowNs();
        const uint64_t host_mono = common::TimeUtil::MonotonicNowNs();

        const uint32_t code = op_result.GetCode();
        if (code == PvResult::Code::ABORTED) {
            // Stop flow in progress; Teardown() collects the remaining buffers.
            return false;
        }
        if (code == PvResult::Code::BUFFER_TOO_SMALL) {
            g_log.Error("[{}] [eBUS] buffer too small for the incoming payload — the device "
                        "payload size changed after buffer allocation; fatal",
                        camera_id_);
            Requeue(buffer);
            stop_->RequestStop(StopReason::kError);
            return false;
        }

        // BlockID gap detection - the authoritative network-loss counter. It runs
        // before any early return: skipping one arrived ID would fabricate a gap
        // on the next frame and double-count it in the emitted-frame rate.
        const uint64_t block_id = buffer->GetBlockID();
        const BlockIdGap gap = block_id_.Observe(block_id);
        uint32_t flags = 0;
        if (gap.gap) {
            stats_->blockid_gap_events.fetch_add(1, std::memory_order_relaxed);
            stats_->frames_lost_gap.fetch_add(gap.missing, std::memory_order_relaxed);
            flags |= format::kFrameFlagBlockIdGap;
        }

        if (!op_result.IsOK()) {
            // Degraded frame: TOO_MANY_RESENDS / RESENDS_FAILURE / IMAGE_ERROR
            // and friends. GetAcquiredSize() bytes are still valid.
            if (output_.on_buffer_error == OnBufferError::kDrop) {
                stats_->frames_error_dropped.fetch_add(1, std::memory_order_relaxed);
                Requeue(buffer);
                return true;
            }
            flags |= format::kFrameFlagIncomplete | format::kFrameFlagResultNotOk;
        }

        // Device timestamp plausibility (recorded verbatim either way).
        const uint64_t device_ts = buffer->GetTimestamp();
        if (device_ts == 0 || (have_last_dts_ && device_ts < last_dts_)) {
            flags |= format::kFrameFlagDeviceTsSuspect;
        }
        if (device_ts != 0) {
            have_last_dts_ = true;
            last_dts_ = device_ts;
        }

        if (max_frames > 0 && recorded_ok_ >= max_frames) {
            Requeue(buffer); // limit already reached (drain path)
            return true;
        }

        const uint64_t acquired = buffer->GetAcquiredSize();
        const uint8_t *src = buffer->GetDataPointer();
        if (acquired != 0 && src == nullptr) {
            stats_->frames_error_dropped.fetch_add(1, std::memory_order_relaxed);
            g_log.Error("[{}] [eBUS] non-empty buffer has no payload pointer; stopping", camera_id_);
            Requeue(buffer);
            stop_->RequestStop(StopReason::kError);
            return false;
        }

        FrameChunkPtr chunk = pool.Acquire();
        if (!chunk) {
            // Writer is behind and the pool is empty: drop_newest semantics keep
            // the PvBuffer flowing back so the NIC never drops whole blocks.
            stats_->frames_dropped_queue.fetch_add(1, std::memory_order_relaxed);
            Requeue(buffer);
            return true;
        }

        if (acquired > chunk->capacity) {
            // The device payload grew past the pool's chunk size: a truncated record is worthless
            if (!oversize_warned_) {
                oversize_warned_ = true;
                g_log.Error("[{}] [eBUS] frame of {} bytes exceeds the chunk capacity {} — dropped (and every "
                            "later one like it)", camera_id_, acquired, chunk->capacity);
            }
            stats_->frames_error_dropped.fetch_add(1, std::memory_order_relaxed);
            pool.Release(std::move(chunk));
            Requeue(buffer);
            return true;
        }
        const size_t copy_n = static_cast<size_t>(acquired);
        if (src != nullptr && copy_n > 0) {
            std::memcpy(chunk->data.get(), src, copy_n);
        }

        FrameMeta meta;
        meta.block_id = block_id;
        meta.device_ts_ns = device_ts;
        meta.host_realtime_ns = host_rt;
        meta.host_monotonic_ns = host_mono;
        meta.payload_type = static_cast<uint32_t>(buffer->GetPayloadType());
        meta.chunk_count = buffer->GetChunkCount();
        meta.operation_result = code;
        if (buffer->GetPayloadType() == PvPayloadTypeImage) {
            PvImage *image = buffer->GetImage();
            if (image != nullptr) {
                meta.pixel_format = static_cast<uint32_t>(image->GetPixelType());
                meta.width = image->GetWidth();
                meta.height = image->GetHeight();
                meta.offset_x = image->GetOffsetX();
                meta.offset_y = image->GetOffsetY();
                meta.padding_x = image->GetPaddingX();
                meta.padding_y = image->GetPaddingY();
            }
        }
        meta.status_flags = flags;
        meta.payload_size = copy_n;
        chunk->meta = meta;

        // Return the PvBuffer to the SDK before touching the (possibly blocking)
        // queue — the SDK pool must never wait on downstream I/O.
        Requeue(buffer);

        const bool block_policy = output_.queue_on_full == QueueOnFull::kBlock;
        bool pushed;
        if (block_policy) {
            // Bounded waits instead of push_blocking(): a stop request (or a
            // writer wedged on a hung filesystem) must never deadlock the
            // acquisition thread. A failed push_wait_for leaves `chunk` intact.
            pushed = queue.push_wait_for(std::move(chunk), std::chrono::milliseconds(100));
            while (!pushed && !queue.closed() && !stop_->StopRequested() &&
                   !local_stop_.load(std::memory_order_relaxed)) {
                pushed = queue.push_wait_for(std::move(chunk), std::chrono::milliseconds(100));
            }
        } else {
            pushed = queue.try_push(std::move(chunk));
        }
        if (!pushed) {
            if (chunk) {
                pool.Release(std::move(chunk));
            }
            stats_->frames_dropped_queue.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        if ((flags & format::kFrameFlagIncomplete) != 0) {
            stats_->frames_incomplete.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_->frames_retrieved_ok.fetch_add(1, std::memory_order_relaxed);
            ++recorded_ok_;
            if (max_frames > 0 && recorded_ok_ >= max_frames) {
                g_log.Info("[{}] [eBUS] max_frames ({}) reached", camera_id_, max_frames);
                stop_->RequestStop(StopReason::kLimitReached);
            }
        }
        stats_->queue_depth.store(queue.size(), std::memory_order_relaxed);

        // Early warning when the SDK pool is running dry (writer falling behind).
        if (buffers_.size() >= 4 && stream_->GetQueuedBufferCount() < buffers_.size() / 4) {
            const uint64_t now = common::TimeUtil::MonotonicNowNs();
            if (now - last_behind_warn_mono_ns_ > 5000000000ull) {
                last_behind_warn_mono_ns_ = now;
                g_log.Warn("[{}] [eBUS] writer falling behind: only {}/{} GVSP buffers queued", camera_id_,
                           stream_->GetQueuedBufferCount(),
                           buffers_.size());
            }
        }
        return true;
    }

    void StreamReceiver::RunAcquisition(ChunkPool &pool, common::BoundedQueue<FrameChunkPtr> &queue,
                                         uint64_t max_frames) {
        stats_->queue_capacity.store(queue.capacity(), std::memory_order_relaxed);

        // Liveness for Main's no-data watchdog. Genuine retrieve timeouts are
        // the "idle" state (expected under an external trigger when the pulses
        // pause), so this loop only reports how long the silence has lasted; how
        // long it MAY last is one rig-wide policy, decided by Main.
        stats_->data_reference_mono_ns.store(common::TimeUtil::MonotonicNowNs(), std::memory_order_release);

        while (!stop_->StopRequested() && !local_stop_.load(std::memory_order_relaxed)) {
            PvBuffer *buffer = nullptr;
            PvResult op_result;
            const PvResult r = stream_->RetrieveBuffer(&buffer, &op_result, kRetrieveTimeoutMs);
            if (!r.IsOK()) {
                if (r.GetCode() == PvResult::Code::TIMEOUT) {
                    continue; // idle, not an error; how long it may last is Main's call
                }
                if (r.GetCode() == PvResult::Code::ABORTED) {
                    break; // stop flow
                }
                g_log.Error("[{}] [eBUS] RetrieveBuffer failed: {}", camera_id_, PvResultToString(r));
                stop_->RequestStop(StopReason::kError);
                break;
            }
            // Before process_buffer: this is the transport's liveness, not the
            // writer's — a stalled queue must not read as a dead camera.
            stats_->data_reference_mono_ns.store(common::TimeUtil::MonotonicNowNs(), std::memory_order_release);
            if (!process_buffer(buffer, op_result, pool, queue, max_frames)) {
                break;
            }
        }

        // Drain: frames already in flight between the stop request and
        // AcquisitionStop taking effect are still recorded. Wall-clock bounded:
        // if the camera is somehow still streaming (AcquisitionStop failed),
        // shutdown must not hang here.
        const uint64_t drain_deadline = common::TimeUtil::MonotonicNowNs() + kDrainMaxTotalNs;
        while (true) {
            if (common::TimeUtil::MonotonicNowNs() >= drain_deadline) {
                g_log.Warn("[{}] [eBUS] drain budget exhausted while frames were still arriving; "
                           "did AcquisitionStop reach the device?",
                           camera_id_);
                break;
            }
            PvBuffer *buffer = nullptr;
            PvResult op_result;
            const PvResult r = stream_->RetrieveBuffer(&buffer, &op_result, kDrainTimeoutMs);
            if (!r.IsOK()) {
                break; // TIMEOUT or ABORTED: nothing more is coming
            }
            if (!process_buffer(buffer, op_result, pool, queue, max_frames)) {
                break;
            }
        }
        stats_->queue_depth.store(queue.size(), std::memory_order_relaxed);
        // Disarm Main's watchdog: this camera is done acquiring, so its silence
        // from here on is expected.
        stats_->data_reference_mono_ns.store(0, std::memory_order_release);
    }

    void StreamReceiver::Teardown() {
        if (torn_down_ || !stream_) {
            torn_down_ = true;
            return;
        }
        torn_down_ = true;

        // AcquisitionStop was already issued by the session; now release
        // TLParamsLocked and collect every in-flight buffer before Close().
        controller_->StreamDisable(/*ignore_errors=*/true);
        if (stream_->IsOpen()) {
            stream_->AbortQueuedBuffers();
            while (stream_->GetQueuedBufferCount() > 0) {
                PvBuffer *buffer = nullptr;
                PvResult op_result;
                const PvResult r = stream_->RetrieveBuffer(&buffer, &op_result, kDrainTimeoutMs);
                if (!r.IsOK()) {
                    g_log.Warn("[{}] [eBUS] retrieve of aborted buffer failed: {}", camera_id_, PvResultToString(r));
                    break;
                }
                // op result ABORTED expected here; buffers are not requeued.
            }
        }
        // Close() first: on the break path above the SDK may still hold queued
        // buffers, and freeing those before the stream is closed is a
        // use-after-free inside the SDK's receive thread.
        if (stream_->IsOpen()) {
            stream_->Close();
        }
        stream_.reset();
        // Destroy the SDK stream before releasing user buffers, including the
        // path where Close()/AbortQueuedBuffers() could not drain the queue.
        buffers_.clear();
        g_log.Info("[{}] [eBUS] stream closed", camera_id_);
    }

    void StreamReceiver::PollStreamStats() {
        if (!stream_) {
            return;
        }
        // Only the two counters that feed the periodic stats line and the
        // Clean() verdict; the session-end full dump covers everything else.
        PvGenParameterArray *sp = stream_->GetParameters();
        int64_t v = 0;
        if (ReadIntFeature(sp, "BlocksDropped", v)) {
            stats_->stream_blocks_dropped.store(static_cast<uint64_t>(v), std::memory_order_relaxed);
        }
        if (ReadIntFeature(sp, "ErrorCount", v)) {
            stats_->stream_error_count.store(static_cast<uint64_t>(v), std::memory_order_relaxed);
        }
    }

    void StreamReceiver::DumpStreamParams(const std::string &path) {
        if (!stream_) {
            return;
        }
        try {
            std::ofstream out(path, std::ios::trunc);
            if (!out) {
                g_log.Warn("[{}] [eBUS] cannot open stream parameter dump {}", camera_id_, path);
                return;
            }
            out << "# PvStream parameter dump: " << camera_id_ << "\n";
            PvGenParameterArray *sp = stream_->GetParameters();
            const uint32_t count = sp != nullptr ? sp->GetCount() : 0;
            for (uint32_t i = 0; i < count; ++i) {
                PvGenParameter *p = sp->Get(i);
                if (p == nullptr) {
                    continue;
                }
                PvString name;
                p->GetName(name);
                std::string value;
                if (!p->IsReadable()) {
                    value = "<not readable>";
                } else {
                    PvString s;
                    value = p->ToString(s).IsOK() ? ToStd(s) : "<error>";
                }
                out << ToStd(name) << " = " << value << "\n";
            }
            g_log.Info("[{}] [eBUS] stream statistics dumped: {}", camera_id_, path);
        } catch (const std::exception &e) {
            g_log.Warn("[{}] [eBUS] stream parameter dump failed: {}", camera_id_, e.what());
        }
    }

    void StreamReceiver::Requeue(PvBuffer *buffer) {
        // A buffer that never re-enters the ring shrinks the pool for good; retry before counting it lost
        PvResult r;
        for (int attempt = 0; attempt < 3; ++attempt) {
            r = stream_->QueueBuffer(buffer);
            if (r.IsOK() || r.GetCode() == PvResult::Code::PENDING) {
                return;
            }
        }
        ++requeue_failures_;
        g_log.Error("[{}] [eBUS] QueueBuffer failed ({}), pool shrank to {} of {} buffers", camera_id_,
                    PvResultToString(r), buffers_.size() - requeue_failures_, buffers_.size());
        if (requeue_failures_ >= buffers_.size()) {
            g_log.Error("[{}] [eBUS] buffer pool exhausted: every QueueBuffer failed — fatal", camera_id_);
            stop_->RequestStop(StopReason::kError);
        }
    }
} // namespace gox::ebus
