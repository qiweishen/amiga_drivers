#include "ebus/stream_receiver.hpp"

#include <PvImage.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "../../include/format.hpp"
#include "logger.h"
#include "../../include/util.hpp"
#include "ebus/sdk_error.hpp"

namespace jai::ebus {
    namespace {
        Common::DriverLog g_log{"GoX"};

        constexpr uint32_t kRetrieveTimeoutMs = 1000;
        constexpr uint32_t kDrainTimeoutMs = 500;
        // Upper bound on the post-stop drain. Without it, a camera that keeps
        // streaming (AcquisitionStop lost/failed on a congested control channel)
        // would feed the drain loop forever and shutdown would hang.
        constexpr uint64_t kDrainMaxTotalNs = 5ull * 1000000000ull;
        constexpr uint32_t kFallbackPacketSize = 1476; // safe on any 1500-MTU path
    } // namespace

    StreamReceiver::StreamReceiver(uint32_t camera_index, const CameraConfig &cfg, CameraController *controller,
                                   StopController *stop, CameraStats *stats) : camera_index_(camera_index),
                                                                               camera_id_(cfg.id),
                                                                               cfg_(cfg),
                                                                               controller_(controller),
                                                                               stop_(stop),
                                                                               stats_(stats) {
    }

    StreamReceiver::~StreamReceiver() {
        try {
            teardown();
        } catch (const std::exception &e) {
            g_log.warn("[{}] stream teardown in destructor failed: {}", camera_id_, e.what());
        }
    }

    void StreamReceiver::open() {
        const StreamConfig &sc = cfg_.stream;
        stream_ = std::make_unique<PvStreamGEV>();

        // 1. Socket receive buffer — must be set before Open() (eBUS 6.5+
        // returns NETWORK_CONFIG_ERROR afterwards). The kernel silently clamps
        // the request to net.core.rmem_max; verified by read-back below.
        rx_buffer_requested_ = sc.socket_rx_buffer_mib * 1024u * 1024u;
        PvResult r = stream_->SetUserModeSocketRxBufferSize(rx_buffer_requested_);
        if (!r.IsOK()) {
            g_log.warn("[{}] SetUserModeSocketRxBufferSize({}) failed: {}", camera_id_, rx_buffer_requested_,
                       pv_result_to_string(r));
        }

        // 2. Open. Port 0 = auto; local_ip pins the stream to one NIC.
        const std::string &device_ip = controller_->identity().ip;
        CHECK_PV(
            stream_->Open(PvString(device_ip.c_str()), 0, static_cast<uint16_t>(sc.channel), PvString(sc.local_ip.c_str(
            ))),
            "PvStreamGEV::Open(" + device_ip + ")");
        local_ip_ = to_std(stream_->GetLocalIPAddress());
        local_port_ = stream_->GetLocalPort();

        uint32_t effective_rx = 0;
        if (stream_->GetUserModeSocketRxBufferSize(effective_rx).IsOK()) {
            rx_buffer_effective_ = effective_rx;
        }
        if (rx_buffer_effective_ != 0 && rx_buffer_effective_ < rx_buffer_requested_) {
            g_log.warn(
                "[{}] socket rx buffer truncated by the kernel: requested {}, effective {}; fix on the host: sysctl -w "
                "net.core.rmem_max={}",
                camera_id_, human_bytes(rx_buffer_requested_), human_bytes(rx_buffer_effective_), rx_buffer_requested_);
        }

        // 3. Packet size (device-side GevSCPSPacketSize, TLParamsLocked-guarded:
        // must happen before StreamEnable).
        PvDeviceGEV *dev = controller_->device();
        if (sc.packet_size == 0) {
            r = dev->NegotiatePacketSize(sc.channel);
            if (!r.IsOK()) {
                g_log.warn("[{}] NegotiatePacketSize failed ({}); falling back to SetPacketSize({})", camera_id_,
                           pv_result_to_string(r),
                           kFallbackPacketSize);
                CHECK_PV(dev->SetPacketSize(kFallbackPacketSize, sc.channel), "PvDeviceGEV::SetPacketSize(fallback)");
            }
        } else {
            r = dev->SetPacketSize(sc.packet_size, sc.channel);
            if (!r.IsOK()) {
                g_log.warn("[{}] SetPacketSize({}) failed ({}); trying negotiation", camera_id_, sc.packet_size,
                           pv_result_to_string(r));
                r = dev->NegotiatePacketSize(sc.channel);
                if (!r.IsOK()) {
                    CHECK_PV(dev->SetPacketSize(kFallbackPacketSize, sc.channel),
                             "PvDeviceGEV::SetPacketSize(fallback)");
                }
            }
        }
        int64_t effective_ps = 0;
        if (read_int_feature(controller_->params(), "GevSCPSPacketSize", effective_ps)) {
            packet_size_ = static_cast<uint32_t>(effective_ps);
        }
        g_log.info("[{}] stream open: device {} -> {}:{} channel {}, packet size {}", camera_id_, device_ip, local_ip_,
                   local_port_,
                   sc.channel, packet_size_);

        // 4. Point the device's stream channel at our receiver.
        CHECK_PV(dev->SetStreamDestination(stream_->GetLocalIPAddress(), stream_->GetLocalPort(), sc.channel),
                 "PvDeviceGEV::SetStreamDestination");

        // 5. Optional inter-packet delay (bandwidth partitioning across cameras).
        if (sc.gev_scpd_ticks > 0) {
            GenicamFeature f;
            f.feature = "GevSCPD";
            f.value = std::to_string(sc.gev_scpd_ticks);
            f.on_error = "warn";
            apply_genicam_feature(controller_->params(), f, cfg_.apply, "[" + camera_id_ + "] device");
        }

        // 6. Receiver-side tuning escape hatch (PvStream GenICam parameters).
        for (const GenicamFeature &f: sc.receiver_tuning) {
            apply_genicam_feature(stream_->GetParameters(), f, cfg_.apply, "[" + camera_id_ + "] stream");
        }
    }

    void StreamReceiver::allocate_buffers() {
        if (!stream_ || !stream_->IsOpen()) {
            throw SdkError("allocate_buffers: stream not open");
        }
        expected_payload_size_ = controller_->payload_size();
        if (expected_payload_size_ == 0) {
            throw SdkError("device reports zero payload size");
        }

        uint32_t count = cfg_.stream.buffer_count;
        if (count == 0) {
            // Auto: absorb ~0.5 s at the configured frame rate, plus headroom.
            if (cfg_.convenience.frame_rate && *cfg_.convenience.frame_rate > 0) {
                const double n = std::ceil(*cfg_.convenience.frame_rate * 0.5) + 8.0;
                count = static_cast<uint32_t>(std::clamp(n, 16.0, 256.0));
            } else {
                count = 32;
            }
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
        g_log.info("[{}] {} GVSP buffers of {} queued ({} total)", camera_id_, count,
                   human_bytes(expected_payload_size_),
                   human_bytes(count * expected_payload_size_));
    }

    bool StreamReceiver::process_buffer(PvBuffer *buffer, const PvResult &op_result, ChunkPool &pool,
                                        BoundedQueue<FrameChunkPtr> &queue, uint64_t max_frames) {
        // Clocks sampled immediately: these bracket the retrieve instant.
        const uint64_t host_rt = now_realtime_ns();
        const uint64_t host_mono = now_monotonic_ns();

        const uint32_t code = op_result.GetCode();
        if (code == PvResult::Code::ABORTED) {
            // Stop flow in progress; teardown() collects the remaining buffers.
            return false;
        }
        if (code == PvResult::Code::BUFFER_TOO_SMALL) {
            g_log.error("[{}] buffer too small for the incoming payload — the device "
                        "payload size changed after buffer allocation; fatal",
                        camera_id_);
            stream_->QueueBuffer(buffer);
            stop_->request_stop(StopReason::Error);
            return false;
        }

        uint32_t flags = 0;
        if (!op_result.IsOK()) {
            // Degraded frame: TOO_MANY_RESENDS / RESENDS_FAILURE / IMAGE_ERROR
            // and friends. GetAcquiredSize() bytes are still valid.
            if (cfg_.recording.on_buffer_error == "drop") {
                stats_->frames_error_dropped.fetch_add(1, std::memory_order_relaxed);
                stream_->QueueBuffer(buffer);
                return true;
            }
            flags |= format::kFrameFlagIncomplete | format::kFrameFlagResultNotOk;
        }

        // BlockID gap detection — the authoritative network-loss counter.
        const uint64_t block_id = buffer->GetBlockID();
        if (have_last_bid_ && block_id > last_bid_ + 1) {
            const uint64_t missing = block_id - last_bid_ - 1;
            stats_->blockid_gap_events.fetch_add(1, std::memory_order_relaxed);
            stats_->frames_lost_gap.fetch_add(missing, std::memory_order_relaxed);
            flags |= format::kFrameFlagBlockIdGap;
        }
        have_last_bid_ = true;
        last_bid_ = block_id;

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
            stream_->QueueBuffer(buffer); // limit already reached (drain path)
            return true;
        }

        const uint64_t acquired = buffer->GetAcquiredSize();
        const uint8_t *src = buffer->GetDataPointer();

        FrameChunkPtr chunk = pool.acquire();
        if (!chunk) {
            // Writer is behind and the pool is empty: drop_newest semantics keep
            // the PvBuffer flowing back so the NIC never drops whole blocks.
            stats_->frames_dropped_queue.fetch_add(1, std::memory_order_relaxed);
            stream_->QueueBuffer(buffer);
            return true;
        }

        const size_t copy_n = static_cast<size_t>(std::min<uint64_t>(acquired, chunk->capacity));
        if (src != nullptr && copy_n > 0) {
            std::memcpy(chunk->data.get(), src, copy_n);
        }

        FrameMeta meta;
        meta.camera_index = camera_index_;
        meta.block_id = block_id;
        meta.device_ts_ns = device_ts;
        meta.host_realtime_ns = host_rt;
        meta.host_monotonic_ns = host_mono;
        if (buffer->GetPayloadType() == PvPayloadTypeImage) {
            PvImage *image = buffer->GetImage();
            if (image != nullptr) {
                meta.pixel_format = static_cast<uint32_t>(image->GetPixelType());
                meta.width = image->GetWidth();
                meta.height = image->GetHeight();
                meta.offset_x = image->GetOffsetX();
                meta.offset_y = image->GetOffsetY();
            }
        }
        meta.status_flags = flags;
        meta.payload_size = copy_n;
        meta.expected_size = expected_payload_size_;
        chunk->meta = meta;

        // Return the PvBuffer to the SDK before touching the (possibly blocking)
        // queue — the SDK pool must never wait on downstream I/O.
        stream_->QueueBuffer(buffer);

        const bool block_policy = cfg_.recording.queue_on_full == "block";
        bool pushed;
        if (block_policy) {
            // Bounded waits instead of push_blocking(): a stop request (or a
            // writer wedged on a hung filesystem) must never deadlock the
            // acquisition thread. A failed push_wait_for leaves `chunk` intact.
            pushed = queue.push_wait_for(std::move(chunk), std::chrono::milliseconds(100));
            while (!pushed && !stop_->stop_requested() && !local_stop_.load(std::memory_order_relaxed)) {
                pushed = queue.push_wait_for(std::move(chunk), std::chrono::milliseconds(100));
            }
        } else {
            pushed = queue.try_push(std::move(chunk));
        }
        if (!pushed) {
            if (chunk) {
                pool.release(std::move(chunk));
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
                g_log.info("[{}] max_frames ({}) reached", camera_id_, max_frames);
                stop_->request_stop(StopReason::LimitReached);
            }
        }
        stats_->queue_depth.store(queue.size(), std::memory_order_relaxed);

        // Early warning when the SDK pool is running dry (writer falling behind).
        if (buffers_.size() >= 4 && stream_->GetQueuedBufferCount() < buffers_.size() / 4) {
            const uint64_t now = now_monotonic_ns();
            if (now - last_behind_warn_mono_ns_ > 5000000000ull) {
                last_behind_warn_mono_ns_ = now;
                g_log.warn("[{}] writer falling behind: only {}/{} GVSP buffers queued", camera_id_,
                           stream_->GetQueuedBufferCount(),
                           buffers_.size());
            }
        }
        return true;
    }

    void StreamReceiver::run_acquisition(ChunkPool &pool, BoundedQueue<FrameChunkPtr> &queue, uint64_t max_frames) {
        stats_->queue_capacity.store(queue.capacity(), std::memory_order_relaxed);

        while (!stop_->stop_requested() && !local_stop_.load(std::memory_order_relaxed)) {
            PvBuffer *buffer = nullptr;
            PvResult op_result;
            const PvResult r = stream_->RetrieveBuffer(&buffer, &op_result, kRetrieveTimeoutMs);
            if (!r.IsOK()) {
                if (r.GetCode() == PvResult::Code::TIMEOUT) {
                    stats_->retrieve_timeouts.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (r.GetCode() == PvResult::Code::ABORTED) {
                    break; // stop flow
                }
                g_log.error("[{}] RetrieveBuffer failed: {}", camera_id_, pv_result_to_string(r));
                stop_->request_stop(StopReason::Error);
                break;
            }
            if (!process_buffer(buffer, op_result, pool, queue, max_frames)) {
                break;
            }
        }

        // Drain: frames already in flight between the stop request and
        // AcquisitionStop taking effect are still recorded. Wall-clock bounded:
        // if the camera is somehow still streaming (AcquisitionStop failed),
        // shutdown must not hang here.
        const uint64_t drain_deadline = now_monotonic_ns() + kDrainMaxTotalNs;
        while (true) {
            if (now_monotonic_ns() >= drain_deadline) {
                g_log.warn("[{}] drain budget exhausted while frames were still arriving; "
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
    }

    void StreamReceiver::teardown() {
        if (torn_down_ || !stream_) {
            torn_down_ = true;
            return;
        }
        torn_down_ = true;

        // AcquisitionStop was already issued by the session; now release
        // TLParamsLocked and collect every in-flight buffer before Close().
        controller_->stream_disable(/*ignore_errors=*/true);
        if (stream_->IsOpen()) {
            stream_->AbortQueuedBuffers();
            while (stream_->GetQueuedBufferCount() > 0) {
                PvBuffer *buffer = nullptr;
                PvResult op_result;
                const PvResult r = stream_->RetrieveBuffer(&buffer, &op_result, kDrainTimeoutMs);
                if (!r.IsOK()) {
                    g_log.warn("[{}] retrieve of aborted buffer failed: {}", camera_id_, pv_result_to_string(r));
                    break;
                }
                // op result ABORTED expected here; buffers are not requeued.
            }
        }
        buffers_.clear(); // frees every PvBuffer (all retrieved by now)
        if (stream_->IsOpen()) {
            stream_->Close();
        }
        stream_.reset();
        g_log.info("[{}] stream closed", camera_id_);
    }

    void StreamReceiver::poll_stream_stats() {
        if (!stream_) {
            return;
        }
        // Only the two counters that feed the periodic stats line and the
        // clean() verdict; the session-end full dump covers everything else.
        PvGenParameterArray *sp = stream_->GetParameters();
        int64_t v = 0;
        if (read_int_feature(sp, "BlocksDropped", v)) {
            stats_->stream_blocks_dropped.store(static_cast<uint64_t>(v), std::memory_order_relaxed);
        }
        if (read_int_feature(sp, "ErrorCount", v)) {
            stats_->stream_error_count.store(static_cast<uint64_t>(v), std::memory_order_relaxed);
        }
    }

    void StreamReceiver::dump_stream_params(const std::string &path) {
        if (!stream_) {
            return;
        }
        try {
            std::ofstream out(path, std::ios::trunc);
            if (!out) {
                g_log.warn("[{}] cannot open stream parameter dump {}", camera_id_, path);
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
                    value = p->ToString(s).IsOK() ? to_std(s) : "<error>";
                }
                out << to_std(name) << " = " << value << "\n";
            }
            g_log.info("[{}] stream statistics dumped: {}", camera_id_, path);
        } catch (const std::exception &e) {
            g_log.warn("[{}] stream parameter dump failed: {}", camera_id_, e.what());
        }
    }

} // namespace jai::ebus
