#include "ebus/stream_receiver.h"

#include <PvBuffer.h>
#include <PvDeviceGEV.h>
#include <PvGenParameter.h>
#include <PvGenParameterArray.h>
#include <PvStream.h>
#include <PvStreamGEV.h>

#include <algorithm>
#include <fstream>
#include <limits>

#include "logger.h"
#include "time_util.h"
#include "buffer_sizing.h"
#include "ebus/discovery.h" // common/ (amiga_ebus)
#include "ebus/sdk_error.h" // common/ (amiga_ebus)


namespace fx10 {
    namespace {
        constexpr std::chrono::seconds kDrainWindow{1}; // stop: silence needed to end the drain
        constexpr std::chrono::seconds kMaxDrainTime{5}; // hard deadline: drain must terminate
        constexpr int kDrainRetrieveTimeoutMs = 200;
        // Buffers keep arriving but none is usable: abort after this long, in
        // BOTH trigger modes. Main's no-data watchdog cannot see this — it only
        // measures silence, and here the SDK never goes silent. Warnings are
        // rate-limited and the retry is paced so a persistent SDK error cannot
        // spin a core or flood the log.
        constexpr double kUnusableAbortS = 10.0;
        constexpr std::chrono::seconds kUnusableWarnInterval{2};
        constexpr std::chrono::milliseconds kUnusableRetryDelay{50};

        common::DriverLog g_log{"FX10"};

        // PvString -> std::string; the shared helper is NULL-safe.
        std::string pv(const PvString &s) { return common::Ebus::ToStd(s); }
    } // namespace


    StreamReceiver::StreamReceiver(const NetworkConfig &network, Counters &counters)
        : network_(network), counters_(counters) {
    }


    StreamReceiver::~StreamReceiver() {
        try {
            Stop();
            Disconnect();
        } catch (...) {
        }
    }


    void StreamReceiver::Connect(const DeviceConfig &device) {
        if (device_ != nullptr) {
            throw TransportError("[eBUS] Already connected");
        }
        // A new connection has a fresh failure state. The previous device was
        // unregistered/freed by Disconnect before any reset recovery retry.
        failed_.store(false);
        link_lost_.store(false);
        fatal_kind_.store(FatalKind::kNone);
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_message_.clear();
        }
        std::string target;
        if (!device.mac.empty()) {
            // Discovery match (common/); an invalid subnet configuration is a
            // hard error naming the remedy - the driver never re-addresses.
            try {
                target = common::Ebus::FindCamera(device.mac).connection_id;
            } catch (const std::exception &e) {
                throw TransportError(std::string("[eBUS] ") + e.what());
            }
        } else {
            target = device.ip;
        }
        if (target.empty()) {
            throw TransportError("[eBUS] No device target: set device.mac or device.ip");
        }
        g_log.Info("[eBUS] Connecting to {} ...", target);

        PvResult result;
        device_ = PvDevice::CreateAndConnect(PvString(target.c_str()), &result);
        if (device_ == nullptr || !result.IsOK()) {
            if (device_ != nullptr) {
                PvDevice::Free(device_);
            }
            device_ = nullptr;
            throw TransportError("[eBUS] Cannot connect to '" + target + "': " + pv(result.GetCodeString()));
        }
        device_gev_ = dynamic_cast<PvDeviceGEV *>(device_);
        if (device_gev_ == nullptr) {
            Disconnect();
            throw TransportError("[eBUS] '" + target + "' is not a GigE Vision device");
        }
        device_->RegisterEventSink(this);
        g_log.Info("[eBUS] Connected");
    }


    void StreamReceiver::OpenStream() {
        if (device_gev_ == nullptr || !device_gev_->IsConnected() || Failed()) {
            throw TransportError("[eBUS] OpenStream: not connected");
        }
        if (stream_ != nullptr) {
            throw TransportError("[eBUS] Stream already open");
        }

        // eBUS 6.5.1 PvStreamGEV.h requires Set -> Open -> Get
        auto stream = std::make_unique<PvStreamGEV>();
        const std::uint32_t rx_bytes = static_cast<std::uint32_t>(network_.socket_rx_buffer_mb) * 1024u * 1024u;
        const PvResult rx_result = stream->SetUserModeSocketRxBufferSize(rx_bytes);
        if (rx_result.GetCode() == PvResult::Code::INVALID_PARAMETER) {
            g_log.Warn("[eBUS] SetUserModeSocketRxBufferSize({} bytes): {}. On Linux, eBUS 6.5.1 "
                       "uses net.core.rmem_max and returns INVALID_PARAMETER when the request exceeds that limit; "
                       "check sysctl net.core.rmem_max in the acquisition environment. Reading back after Open",
                       rx_bytes, common::Ebus::PvResultToString(rx_result));
        } else if (!rx_result.IsOK()) {
            g_log.Warn("[eBUS] SetUserModeSocketRxBufferSize({} bytes) failed: {}; "
                       "requested capacity is not confirmed", rx_bytes, common::Ebus::PvResultToString(rx_result));
        }

        // The explicit GEV Open overload expects an IP, whereas Connect also
        // accepts a MAC/device ID. Use the connected device's actual address.
        const PvResult result = stream->Open(device_gev_->GetIPAddress());
        if (!result.IsOK()) {
            throw TransportError("[eBUS] Cannot open stream: " + pv(result.GetCodeString()));
        }

        std::uint32_t rx_readback = 0;
        const PvResult rx_read = stream->GetUserModeSocketRxBufferSize(rx_readback);
        runtime_metadata_["socket_rx_requested_bytes"] = rx_bytes;
        runtime_metadata_["socket_rx_set_result"] = common::Ebus::PvResultToString(rx_result);
        runtime_metadata_["socket_rx_read_result"] = common::Ebus::PvResultToString(rx_read);
        runtime_metadata_["socket_rx_effective_bytes"] = rx_read.IsOK() ? nlohmann::json(rx_readback) : nlohmann::json(nullptr);
        runtime_metadata_["socket_rx_semantics"] = "SDK SO_RCVBUF readback; Linux may include doubled bookkeeping; not payload capacity";
        if (rx_read.IsOK()) {
            g_log.Info("[eBUS] socket rx buffer: requested={} bytes, SDK SO_RCVBUF readback={} bytes, "
                       "set_result={}; Linux readback may include doubled bookkeeping allocation",
                       rx_bytes, rx_readback, pv(rx_result.GetCodeString()));
            if (rx_readback < rx_bytes) {
                g_log.Warn("[eBUS] socket rx readback is below the requested size; "
                           "receive buffering is smaller than configured");
            }
        } else if (rx_read.GetCode() == PvResult::Code::NOT_SUPPORTED) {
            g_log.Info("[eBUS] socket rx readback: {}; this API only supports the user-mode receiver, "
                       "socket capacity is unavailable", common::Ebus::PvResultToString(rx_read));
        } else {
            g_log.Warn("[eBUS] GetUserModeSocketRxBufferSize failed: {}; socket capacity is unknown",
                       common::Ebus::PvResultToString(rx_read));
        }

        if (network_.packet_size > 0) {
            // Fixed packet size for a known network path.
            PvGenInteger *packet_size = dynamic_cast<PvGenInteger *>(device_->GetParameters()->Get("GevSCPSPacketSize"));
            if (packet_size == nullptr || !packet_size->SetValue(network_.packet_size).IsOK()) {
                g_log.Warn("[eBUS] Cannot set GevSCPSPacketSize={}, falling back to negotiation", network_.packet_size);
                device_gev_->NegotiatePacketSize();
            }
        } else {
            const PvResult negotiated = device_gev_->NegotiatePacketSize();
            if (!negotiated.IsOK()) {
                g_log.Warn("[eBUS] Packet size negotiation failed ({}), device default in effect",
                           pv(negotiated.GetCodeString()));
            }
        }

        const PvResult dest = device_gev_->SetStreamDestination(stream->GetLocalIPAddress(),
                                                                stream->GetLocalPort());
        if (!dest.IsOK()) {
            throw TransportError("[eBUS] SetStreamDestination failed: " + pv(dest.GetCodeString()));
        }
        g_log.Trace("[eBUS] Stream open on {}:{}", pv(stream->GetLocalIPAddress()), stream->GetLocalPort());
        stream_gev_ = stream.get();
        stream_ = stream.release();
    }


    void StreamReceiver::Start(IFrameSink &sink, const ExpectedGeometry &expected, double expected_fps,
                               const std::function<void()> &before_acquisition) {
        if (stream_ == nullptr) {
            throw TransportError("[eBUS] Start: stream not open");
        }
        if (streaming_.load() || acquisition_thread_.joinable() || pipeline_ || !buffers_.empty()) {
            throw TransportError("[eBUS] Start requires the previous acquisition to be stopped and drained");
        }
        // Wire size per frame: packed formats carry fewer bytes than W*H*bpp
        pixel_info_ = GetPixelFormatInfo(expected.pixel_format);
        if (expected.width == 0 || expected.height == 0 || pixel_info_ == nullptr ||
            expected.bytes_per_pixel != pixel_info_->storage_bpp ||
            expected.height > std::numeric_limits<std::size_t>::max() / expected.width / 2) {
            throw TransportError("[eBUS] Invalid or overflowing expected image geometry");
        }
        const std::size_t pixels = static_cast<std::size_t>(expected.width) * expected.height;
        const std::size_t wire_bytes =
                pixel_info_ != nullptr
                    ? WireBytes(*pixel_info_, pixels)
                    : pixels * expected.bytes_per_pixel;
        if (expected.width == 0 || expected.height == 0 || pixel_info_ == nullptr ||
            expected.payload_size < wire_bytes || (pixel_info_->packed && expected.width % 2 != 0)) {
            throw TransportError(
                "[eBUS] Expected geometry inconsistent: payload " + std::to_string(expected.payload_size) +
                "; minimum " + std::to_string(wire_bytes) + " pixel bytes for " + std::to_string(expected.width) +
                "x" + std::to_string(expected.height) + " " + expected.pixel_format +
                " (requires a supported format, nonzero dimensions and complete packed pixel pairs per row)");
        }
        // PayloadSize sizes the receive buffers. Only a received image can tell
        // us whether extra bytes are declared padding or unsupported data.
        if (expected.payload_size != wire_bytes) {
            g_log.Trace("[eBUS] Receive payload {} B, effective pixels {} B; validating received image layout",
                        expected.payload_size, wire_bytes);
        }
        expected_ = expected;
        first_frame_checked_ = false;
        tracker_ = BlockIdTracker();
        // Full state reset: this object supports reconnect cycles (stop -> disconnect -> connect -> start);
        // so stale link-loss/error state must not survive
        stop_requested_.store(false);
        failed_.store(false);
        fatal_kind_.store(FatalKind::kNone);
        link_lost_.store(false);
        loss_seen_.store(false);
        acq_stop_sent_.store(false);
        frames_delivered_.store(0);
        last_frame_us_.store(0);
        stream_start_us_.store(common::TimeUtil::SteadyNowUs());
        queue_failures_ = 0;
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_message_.clear();
        }

        const std::uint32_t payload = device_->GetPayloadSize();
        if (payload != expected.payload_size) {
            // The expected geometry came from ReadGeometry() moments ago;
            // a mismatch means the camera state changed since — the first-frame check would abort anyway,
            // so fail fast before enabling the stream.
            throw TransportError("[eBUS] Device PayloadSize " + std::to_string(payload) +
                                 " != expected " + std::to_string(expected.payload_size) +
                                 " (camera state changed after configuration?)");
        }
        const auto canonical_bytes = ((pixels * expected.bytes_per_pixel + 1) / 2) * 2;
        const auto plan = PlanRecordingBuffers(network_, expected_fps, payload, canonical_bytes,
                                              stream_->GetQueuedBufferMaximum());
        if (plan.sdk_buffers == 0 || plan.queue_frames == 0) {
            throw TransportError("[eBUS] Buffer memory limit cannot fit the receive and recording pools");
        }
        const auto total_bytes = plan.sdk_bytes + plan.application_bytes + plan.canonical_bytes;
        if (plan.clamped_by_memory || plan.clamped_by_stream) {
            g_log.Warn("[eBUS] Recording buffers clamped: SDK={}, queue={}, achievable queue stall {:.2f} s",
                       plan.sdk_buffers, plan.queue_frames, plan.achievable_stall_s);
        }
        g_log.Info("[eBUS] Recording pipeline: SDK={} buffers, application={} queued frames, "
                   "payload/scratch memory={:.1f} MiB, queue stall budget={:.2f} s",
                   plan.sdk_buffers, plan.queue_frames, static_cast<double>(total_bytes) / 1048576.0,
                   plan.achievable_stall_s);

        buffers_.reserve(plan.sdk_buffers);
        for (std::uint32_t i = 0; i < plan.sdk_buffers; ++i) {
            auto buffer = std::make_unique<PvBuffer>();
            const PvResult alloc = buffer->Alloc(payload);
            if (!alloc.IsOK()) {
                FreeBuffers();
                throw TransportError(
                    "[eBUS] Buffer allocation failed at " + std::to_string(i) + "/" + std::to_string(plan.sdk_buffers) + ": "
                    + pv(
                        alloc.GetCodeString()
                    )
                );
            }
            buffers_.push_back(std::move(buffer));
        }
        for (auto &buffer: buffers_) {
            const PvResult queued = stream_->QueueBuffer(buffer.get());
            // PENDING = buffer accepted into the input queue, fill outstanding
            if (!queued.IsOK() && queued.GetCode() != PvResult::Code::PENDING) {
                if (DrainAborted()) {
                    FreeBuffers();
                }
                throw TransportError("[eBUS] Initial QueueBuffer failed: " + pv(queued.GetCodeString()));
            }
        }

        RecordingPipeline::Options pipeline_options;
        pipeline_options.width = expected.width;
        pipeline_options.height = expected.height;
        pipeline_options.pixel_format = expected.pixel_format;
        pipeline_options.status_line = expected.status_line;
        pipeline_options.payload_capacity = payload;
        pipeline_options.queue_frames = plan.queue_frames;
        pipeline_ = std::make_unique<RecordingPipeline>(sink, std::move(pipeline_options));

        runtime_metadata_["buffer_count"] = buffers_.size();
        runtime_metadata_["buffer_bytes"] = plan.sdk_bytes;
        runtime_metadata_["recording_queue_frames"] = plan.queue_frames;
        runtime_metadata_["recording_pool_bytes"] = plan.application_bytes;
        runtime_metadata_["canonical_scratch_bytes"] = plan.canonical_bytes;
        runtime_metadata_["total_payload_buffer_bytes"] = total_bytes;
        runtime_metadata_["payload_capacity_bytes"] = payload;
        runtime_metadata_["estimated_stall_seconds"] = plan.achievable_stall_s;
        runtime_metadata_["stall_rate_basis_hz"] = expected_fps;
        runtime_metadata_["stall_estimate_scope"] = "application queue only; SDK slack is not added";
        runtime_metadata_["clamped_by_memory"] = plan.clamped_by_memory;
        runtime_metadata_["clamped_by_stream"] = plan.clamped_by_stream;
        runtime_metadata_["sink_execution"] = "dedicated unpack/ENVI writer; owned SDK payload copies in bounded queue";
        runtime_metadata_["recording_queue_full_policy"] = "fatal; drain accepted events and stop session";
        if (before_acquisition) before_acquisition();

        PvResult result = device_->StreamEnable();
        if (!result.IsOK()) {
            if (DrainAborted()) {
                FreeBuffers();
            }
            throw TransportError("[eBUS] StreamEnable failed: " + pv(result.GetCodeString()));
        }
        PvGenCommand *start_command = dynamic_cast<PvGenCommand *>(device_->GetParameters()->Get("AcquisitionStart"));
        if (start_command == nullptr || !start_command->Execute().IsOK()) {
            device_->StreamDisable();
            if (DrainAborted()) {
                FreeBuffers();
            }
            throw TransportError("[eBUS] AcquisitionStart failed");
        }

        try {
            acquisition_thread_ = std::thread([this] {
                try { AcquisitionLoop(); }
                catch (const std::exception &error) {
                    LatchFatal(std::string("acquisition worker failed: ") + error.what());
                } catch (...) {
                    LatchFatal("acquisition worker failed with an unknown exception");
                }
            });
        } catch (...) {
            // Unwind like the AcquisitionStart failure path
            // Never leave the camera acquiring with nobody retrieving
            ExecuteAcquisitionStop();
            device_->StreamDisable();
            if (DrainAborted()) {
                FreeBuffers();
            }
            throw;
        }
        streaming_.store(true);
        g_log.Info("[eBUS] Acquisition started");
    }


    void StreamReceiver::StopAcquisition() {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        if (!device_ || link_lost_.load()) {
            throw TransportError("[eBUS] Cannot stop acquisition: control link unavailable");
        }
        if (acq_stop_sent_.exchange(true)) {
            if (failed_.load()) throw TransportError(ErrorMessage());
            return;
        }
        auto *command = dynamic_cast<PvGenCommand *>(device_->GetParameters()->Get("AcquisitionStop"));
        if (!command || !command->Execute().IsOK()) {
            LatchFatal("AcquisitionStop was not acknowledged; phase boundary is unconfirmed", FatalKind::kOther);
            throw TransportError(ErrorMessage());
        }
    }


    void StreamReceiver::ExecuteAcquisitionStop() {
        if (acq_stop_sent_.exchange(true)) {
            return; // once per run
        }
        PvGenCommand *stop_command = dynamic_cast<PvGenCommand *>(device_->GetParameters()->Get("AcquisitionStop"));
        if (stop_command == nullptr || !stop_command->Execute().IsOK()) {
            g_log.Warn("[eBUS] AcquisitionStop failed (continuing teardown)");
        }
    }


    void StreamReceiver::Requeue(PvBuffer *buffer) {
        // A buffer that never re-enters the ring shrinks the pool for good; retry before counting it lost
        PvResult result;
        for (int attempt = 0; attempt < 3; ++attempt) {
            result = stream_->QueueBuffer(buffer);
            if (result.IsOK() || result.GetCode() == PvResult::Code::PENDING) {
                return;
            }
        }
        ++queue_failures_;
        ++counters_.op_errors;
        loss_seen_.store(true, std::memory_order_release);
        g_log.Error("[eBUS] QueueBuffer failed ({}), pool shrank by {} buffer(s)", pv(result.GetCodeString()),
                    queue_failures_);
        if (queue_failures_ >= buffers_.size()) {
            LatchFatal("buffer pool exhausted: every QueueBuffer failed");
        }
    }


    // True when a fatal was latched. `last_arrival` is when the SDK last
    // answered at all, `last_frame` when it last produced a USABLE frame.
    bool StreamReceiver::CheckStreamUnusable(std::chrono::steady_clock::time_point last_arrival,
                                              std::chrono::steady_clock::time_point last_frame) {
        const double unusable_s = std::chrono::duration<double>(last_arrival - last_frame).count();
        if (unusable_s <= kUnusableAbortS) {
            return false;
        }
        LatchFatal("stream unusable: buffers kept arriving for " + std::to_string(unusable_s) +
                    " s without a single usable frame (operation errors, non-image payloads, or a "
                    "persistent RetrieveBuffer failure)",
                    FatalKind::kStreamUnusable);
        return true;
    }


    bool StreamReceiver::SubmitFrame(const FrameView &frame, std::uint64_t first_missing,
                                     std::uint64_t missing_count, RecordingPipeline::Rejection rejection) {
        if (pipeline_->Submit(frame, first_missing, missing_count, rejection)) return true;
        if (rejection == RecordingPipeline::Rejection::kNone) ++counters_.recording_queue_drops;
        loss_seen_.store(true, std::memory_order_release);
        LatchFatal(pipeline_->ErrorMessage(), FatalKind::kRecordingPipeline);
        return false;
    }

    void StreamReceiver::AcquisitionLoop() {
        using clock = std::chrono::steady_clock;
        auto last_frame = clock::now();

        bool draining = false;
        auto silence_start = clock::now();
        auto drain_deadline = clock::now();
        // last_frame advances on a USABLE frame, last_arrival on anything the SDK hands over:
        // "everything unusable" is fatal here, total silence is Main's call (LastDataReferenceUs)
        auto last_arrival = clock::now();
        auto next_unusable_warn = clock::now();

        while (true) {
            const bool fatal = failed_.load() || link_lost_.load();
            if (fatal) {
                // A fatal condition ends the loop immediately, drain included;
                // the AcquisitionStop for the live-link fatal cases is issued in Stop()
                break;
            }
            if (pipeline_->Failed()) {
                LatchFatal(pipeline_->ErrorMessage(), FatalKind::kRecordingPipeline);
                break;
            }
            if (!draining && stop_requested_.load()) {
                ExecuteAcquisitionStop();
                draining = true;
                silence_start = clock::now();
                drain_deadline = clock::now() + kMaxDrainTime;
                g_log.Trace("[eBUS] Acquisition stop requested; Draining in-flight frames");
            }
            if (draining && clock::now() >= drain_deadline) {
                g_log.Warn(
                    "[eBUS] Drain deadline reached with frames still arriving (camera did not honor AcquisitionStop?); forcing teardown");
                break;
            }

            PvBuffer *buffer = nullptr;
            PvResult op_result;
            const PvResult result = stream_->RetrieveBuffer(
                &buffer,
                &op_result,
                draining ? kDrainRetrieveTimeoutMs : static_cast<std::uint32_t>(network_.retrieve_timeout_ms)
            );

            if (!result.IsOK()) {
                if (draining) {
                    if (clock::now() - silence_start >= kDrainWindow) {
                        break;
                    }
                    continue;
                }
                // Only genuine timeouts are the "idle" state (expected under external trigger when pulses pause);
                // Other codes are stream errors
                if (result.GetCode() != PvResult::Code::TIMEOUT) {
                    ++counters_.op_errors;
                    loss_seen_.store(true, std::memory_order_release);
                    last_arrival = clock::now(); // the SDK answered, just not with a frame
                    if (clock::now() >= next_unusable_warn) {
                        next_unusable_warn = clock::now() + kUnusableWarnInterval;
                        g_log.Warn("[eBUS] RetrieveBuffer error: {} (rate-limited)", pv(result.GetCodeString()));
                    }
                    if (CheckStreamUnusable(last_arrival, last_frame)) {
                        continue;
                    }
                    std::this_thread::sleep_for(kUnusableRetryDelay); // do not spin on a persistent error
                    continue;
                }
                ++counters_.retrieve_timeouts;
                // A timeout is the idle state, not an error: under an external
                // trigger it just means no pulse arrived. How long the silence
                // may last is Main's policy (Guards: in config-main.yaml).
                continue;
            }

            // We hold a buffer from here: it MUST be re-queued on every path.
            last_arrival = clock::now();
            const auto host_receive_rt = common::TimeUtil::RealtimeNowNs();
            const auto host_receive_mono = common::TimeUtil::MonotonicNowNs();
            FrameView frame;
            frame.block_id = buffer->GetBlockID();
            frame.device_timestamp_raw = buffer->GetTimestamp();
            frame.host_receive_realtime_ns = host_receive_rt;
            frame.host_receive_monotonic_ns = host_receive_mono;
            frame.sdk.emplace();
            auto &sdk = *frame.sdk;
            sdk.acquired_size = buffer->GetAcquiredSize();
            sdk.payload_type = static_cast<std::uint32_t>(buffer->GetPayloadType());
            sdk.operation_result = static_cast<std::uint32_t>(op_result.GetCode());
            sdk.chunk_count = buffer->GetChunkCount();
            if (buffer->GetPayloadType() == PvPayloadTypeImage) {
                if (auto *image = buffer->GetImage()) {
                    sdk.image_present = true;
                    sdk.pixel_type = static_cast<std::uint32_t>(image->GetPixelType());
                    sdk.width = image->GetWidth();
                    sdk.height = image->GetHeight();
                    sdk.padding_x = image->GetPaddingX();
                    sdk.padding_y = image->GetPaddingY();
                    sdk.image_size = image->GetImageSize();
                    sdk.effective_image_size = image->GetEffectiveImageSize();
                }
            }
            if (!op_result.IsOK()) {
                ++counters_.op_errors;
                loss_seen_.store(true, std::memory_order_release);
                g_log.Warn("[eBUS] Buffer operation error: {}", pv(op_result.GetCodeString()));
                SubmitFrame(frame, 0, 0, RecordingPipeline::Rejection::kOperationError);
                Requeue(buffer);
                CheckStreamUnusable(last_arrival, last_frame);
                continue;
            }
            if (buffer->GetPayloadType() != PvPayloadTypeImage) {
                ++counters_.op_errors;
                loss_seen_.store(true, std::memory_order_release);
                g_log.Warn("[eBUS] Non-image payload type {} dropped", static_cast<int>(buffer->GetPayloadType()));
                SubmitFrame(frame, 0, 0, RecordingPipeline::Rejection::kNonImage);
                Requeue(buffer);
                CheckStreamUnusable(last_arrival, last_frame);
                continue;
            }
            if (!CheckFrame(*buffer)) {
                SubmitFrame(frame, 0, 0, RecordingPipeline::Rejection::kLayoutError);
                Requeue(buffer);
                continue; // latchFatal_ set; loop head exits
            }
            first_frame_checked_ = true;

            const auto observation = tracker_.Observe(buffer->GetBlockID());
            if (observation.anomaly) {
                ++counters_.blockid_anomalies;
                loss_seen_.store(true, std::memory_order_release);
                g_log.Warn("[eBUS] BlockID anomaly at {}", buffer->GetBlockID());
            }
            if (observation.gap_before > 0) {
                loss_seen_.store(true, std::memory_order_release);
            }

            ++counters_.retrieve_ok;
            last_frame = clock::now();
            // Before the sink: this is the transport's liveness, not the writer's.
            last_frame_us_.store(common::TimeUtil::SteadyNowUs(), std::memory_order_release);

            PvImage *image = buffer->GetImage();
            frame.data = image->GetDataPointer();
            frame.size = static_cast<std::size_t>(sdk.acquired_size);
            frame.width = image != nullptr ? image->GetWidth() : 0;
            frame.height = image != nullptr ? image->GetHeight() : 0;
            frame.bytes_per_pixel = expected_.bytes_per_pixel;
            frame.block_id = buffer->GetBlockID();
            frame.device_timestamp_raw = buffer->GetTimestamp();
            frame.host_receive_realtime_ns = host_receive_rt;
            frame.host_receive_monotonic_ns = host_receive_mono;
            frame.block_id_anomaly = observation.anomaly;

            // Submit owns the bytes before returning. The SDK buffer is never
            // held across unpacking, disk writes, flushes or segment rotation.
            if (SubmitFrame(frame, observation.first_missing, observation.gap_before))
                frames_delivered_.fetch_add(1);
            Requeue(buffer);

            if (draining) {
                silence_start = clock::now(); // still receiving: extend the drain
            }
        }
        streaming_.store(false);
    }


    bool StreamReceiver::CheckFrame(PvBuffer &buffer) {
        PvImage *image = buffer.GetImage();
        const std::uint32_t width = image != nullptr ? image->GetWidth() : 0;
        const std::uint32_t height = image != nullptr ? image->GetHeight() : 0;
        const std::uint64_t size = buffer.GetAcquiredSize();
        if (image == nullptr || width != expected_.width || height != expected_.height || size > expected_.payload_size) {
            LatchFatal("frame mismatch: got " + std::to_string(width) + "x" +
                        std::to_string(height) + " (" + std::to_string(size) + " B), expected " +
                        std::to_string(expected_.width) + "x" + std::to_string(expected_.height) + " (" +
                        std::to_string(expected_.payload_size) +
                        " B). Check binning/MROI/status_line/pixel_format vs the camera state.",
                        FatalKind::kFirstFrame);
            return false;
        }

        const std::uint64_t row_bytes = WireBytes(*pixel_info_, width);
        const std::uint64_t effective = row_bytes * height;
        const std::uint64_t padded = (row_bytes + image->GetPaddingX()) * height + image->GetPaddingY();
        if (!first_frame_checked_) {
            g_log.Info("[eBUS] Received layout: {}x{}, acquired={} B, image={} B, effective={} B, "
                       "paddingX={} B, paddingY={} B, chunks={}, offsets={},{}",
                       width, height, size, image->GetImageSize(), image->GetEffectiveImageSize(),
                       image->GetPaddingX(), image->GetPaddingY(), buffer.GetChunkCount(),
                       image->GetOffsetX(), image->GetOffsetY());
        }
        // Do not infer padding from a byte-count difference. Require the SDK's
        // image metadata to account for every received byte before touching pixels.
        if (image->GetDataPointer() == nullptr || buffer.GetChunkCount() != 0 ||
            image->GetEffectiveImageSize() != effective || image->GetImageSize() != padded ||
            size != padded || image->IsPartialLineMissing() || image->IsFullLineMissing() ||
            image->IsDataOverrun()) {
            LatchFatal("unsupported or incomplete image layout: acquired=" + std::to_string(size) +
                       ", image=" + std::to_string(image->GetImageSize()) +
                       ", effective=" + std::to_string(image->GetEffectiveImageSize()) +
                       ", expected pixels=" + std::to_string(effective) +
                       ", paddingX=" + std::to_string(image->GetPaddingX()) +
                       ", paddingY=" + std::to_string(image->GetPaddingY()) +
                       ", chunks=" + std::to_string(buffer.GetChunkCount()) +
                       "; unknown bytes and missing image data cannot be recorded as pixels",
                       FatalKind::kFirstFrame);
            return false;
        }

        // Pixel-type assertion (plan risk #2: packed/16-bit formats would corrupt the
        // cube even when the byte size happens to match).
        if (image != nullptr && !expected_.pixel_format.empty()) {
            const PvPixelType actual = image->GetPixelType();
            PvPixelType wanted = PvPixelUndefined;
            if (expected_.pixel_format == "Mono8") {
                wanted = PvPixelMono8;
            } else if (expected_.pixel_format == "Mono10") {
                wanted = PvPixelMono10;
            } else if (expected_.pixel_format == "Mono10Packed") {
                wanted = PvPixelMono10Packed;
            } else if (expected_.pixel_format == "Mono12") {
                wanted = PvPixelMono12;
            } else if (expected_.pixel_format == "Mono12Packed") {
                wanted = PvPixelMono12Packed;
            }
            if (wanted != PvPixelUndefined && actual != wanted) {
                LatchFatal("frame pixel type mismatch: camera delivers type " +
                            std::to_string(static_cast<int>(actual)) + ", configured " +
                            expected_.pixel_format,
                            FatalKind::kFirstFrame);
                return false;
            }
        }

        if (!first_frame_checked_) {
            g_log.Trace("[eBUS] First frame OK: {}x{} ({} B), block_id {}", width, height, size, buffer.GetBlockID());
        }
        return true;
    }


    void StreamReceiver::Stop() {
        std::lock_guard<std::mutex> lock(stop_mutex_); // Disconnect() and the destructor may race
        if (!streaming_.load() && !acquisition_thread_.joinable() && !pipeline_) {
            return;
        }
        stop_requested_.store(true);
        if (acquisition_thread_.joinable()) {
            acquisition_thread_.join();
        }

        // Fatal exits (first-frame mismatch, watchdog, sink failure) break the loop
        // before it can execute AcquisitionStop; issue it here while the control
        // channel is still alive. No-op after a normal drain (already sent) and
        // pointless after link loss (best effort, will just warn).
        if (device_ != nullptr && !link_lost_.load()) {
            ExecuteAcquisitionStop();
        }
        if (device_ != nullptr) {
            device_->StreamDisable();
        }
        if (stream_gev_ != nullptr) {
            stream_gev_->FlushPacketQueue();
        }
        if (DrainAborted()) {
            FreeBuffers();
        } else {
            // The stream may still reference buffers; Disconnect() frees them
            // unconditionally after Close().
            g_log.Warn("[eBUS] Stream queue not fully drained; buffer release deferred to disconnect()");
        }
        if (pipeline_) {
            pipeline_->Finish(); // receiver joined; drain copies before recorder.Stop()
            counters_.recording_worker_unconfirmed += pipeline_->FramesUnconfirmed();
            if (pipeline_->Failed()) LatchFatal(pipeline_->ErrorMessage(), FatalKind::kRecordingPipeline);
            g_log.Info("[Writer] Queue drained: accepted={}, delivered_to_sink={}, unconfirmed={} "
                       "(unconfirmed may overlap sink deliveries after I/O failure)",
                       frames_delivered_.load(), pipeline_->FramesDelivered(), pipeline_->FramesUnconfirmed());
            pipeline_.reset();
        }
        streaming_.store(false);
        g_log.Info("[eBUS] Acquisition stopped ({} frames admitted to recording queue)", frames_delivered_.load());
    }


    bool StreamReceiver::DrainAborted() const {
        if (stream_ == nullptr) return true;
        stream_->AbortQueuedBuffers();
        while (stream_->GetQueuedBufferCount() > 0) {
            PvBuffer *buffer = nullptr;
            PvResult op_result;
            if (!stream_->RetrieveBuffer(&buffer, &op_result, kDrainRetrieveTimeoutMs).IsOK()) {
                return stream_->GetQueuedBufferCount() == 0;
            }
            // Aborted buffers are NOT delivered to the sink.
        }
        return true;
    }


    void StreamReceiver::FreeBuffers() { buffers_.clear(); }


    void StreamReceiver::Disconnect() {
        Stop(); // also joins a pipeline created during a partially failed Start
        if (stream_ != nullptr) {
            stream_->Close();
            delete stream_; // allocated locally with new, not PvStream::CreateAndOpen
            stream_ = nullptr;
            stream_gev_ = nullptr;
        }
        FreeBuffers(); // safe now: the stream (if any) is closed
        if (device_ != nullptr) {
            device_->UnregisterEventSink(this);
            device_->Disconnect();
            PvDevice::Free(device_);
            device_ = nullptr;
            device_gev_ = nullptr;
        }
    }

    bool StreamReceiver::DumpStreamParams(const std::filesystem::path &path) const {
        if (!stream_ || streaming_.load()) return false;
        try {
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out) throw TransportError("cannot open " + path.string());
            out << "# FX10 PvStream parameter snapshot after Stop, before Disconnect\n"
                << "# host_realtime_ns=" << common::TimeUtil::RealtimeNowNs() << "\n"
                << "# SDK counters, not the application's frame ledger; not a no-loss certificate\n";
            auto *params = stream_->GetParameters();
            if (!params) out << "# parameters unavailable\n";
            const auto count = params ? params->GetCount() : 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                auto *p = params->Get(i);
                if (!p) continue;
                PvString name, value;
                p->GetName(name);
                out << pv(name) << " = ";
                if (!p->IsReadable()) out << "<not readable>";
                else if (!p->ToString(value).IsOK()) out << "<read error>";
                else out << pv(value);
                out << '\n';
            }
            out.close();
            if (!out) throw TransportError("cannot write/close " + path.string());
            return true;
        } catch (const std::exception &e) {
            g_log.Error("[eBUS] Stream statistics dump failed: {}", e.what());
            return false;
        }
    }


    void StreamReceiver::LatchFatal(const std::string &message, FatalKind kind) {
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            if (failed_.load()) return;
            error_message_ = message;
        }
        fatal_kind_.store(kind);
        failed_.store(true);
        g_log.Error("[eBUS] Transport fatal: {}", message);
    }


    std::optional<std::uint64_t> StreamReceiver::LastDataReferenceUs() const {
        if (!streaming_.load()) {
            return std::nullopt; // not acquiring: silence carries no information
        }
        const auto last = last_frame_us_.load(std::memory_order_acquire);
        // No frame yet: measure from the moment acquisition started, so a camera
        // that accepts AcquisitionStart and then sends nothing is caught too.
        const auto reference = last != 0 ? last : stream_start_us_.load(std::memory_order_acquire);
        return reference != 0 ? std::optional<std::uint64_t>(reference) : std::nullopt;
    }


    std::string StreamReceiver::ErrorMessage() const {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return error_message_;
    }


    void StreamReceiver::OnLinkDisconnected(PvDevice *) {
        link_lost_.store(true);
        LatchFatal("SDK reported control link disconnection", FatalKind::kLinkLost);
    }
} // namespace fx10
