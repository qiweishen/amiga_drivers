#include "ebus/stream_receiver.hpp"

#include <PvBuffer.h>
#include <PvDeviceGEV.h>
#include <PvDeviceInfoGEV.h>
#include <PvGenParameter.h>
#include <PvGenParameterArray.h>
#include <PvInterface.h>
#include <PvStream.h>
#include <PvStreamGEV.h>
#include <PvSystem.h>

#include <algorithm>

#include "logger.h"
#include "string_util.h"
#include "buffer_sizing.hpp"


namespace fx10 {
    namespace {
        constexpr std::chrono::seconds kDrainWindow{1}; // stop: silence needed to end the drain
        constexpr std::chrono::seconds kMaxDrainTime{5}; // hard deadline: drain must terminate
        constexpr int kDrainRetrieveTimeoutMs = 200;
        constexpr std::chrono::milliseconds kMacDiscoveryTimeout{4000};

        Common::DriverLog g_log{"FX10"};

        std::string pv(const PvString &s) { return std::string(s.GetAscii()); }
    } // namespace


    std::vector<DiscoveredDevice> discoverDevices(std::chrono::milliseconds timeout) {
        std::vector<DiscoveredDevice> devices;
        PvSystem system;
        system.SetDetectionTimeout(static_cast<std::uint32_t>(timeout.count()));
        const PvResult result = system.Find();
        if (!result.IsOK()) {
            throw TransportError("Device discovery failed: " + pv(result.GetCodeString()));
        }
        for (std::uint32_t i = 0; i < system.GetInterfaceCount(); ++i) {
            const PvInterface *iface = system.GetInterface(i);
            if (iface == nullptr) {
                continue;
            }
            for (std::uint32_t d = 0; d < iface->GetDeviceCount(); ++d) {
                const PvDeviceInfo *info = iface->GetDeviceInfo(d);
                if (info == nullptr) {
                    continue;
                }
                DiscoveredDevice device;
                device.display_id = pv(info->GetDisplayID());
                device.connection_id = pv(info->GetConnectionID());
                device.configuration_valid = info->IsConfigurationValid();
                if (const auto *gev = dynamic_cast<const PvDeviceInfoGEV *>(info)) {
                    device.ip = pv(gev->GetIPAddress());
                    device.mac = pv(gev->GetMACAddress());
                }
                devices.push_back(std::move(device));
            }
        }
        return devices;
    }


    namespace {
        // Resolve device.mac to its eBUS connection ID via discovery. When the
        // camera sits on a wrong subnet and device.force_ip is enabled, a FORCEIP
        // is sent first (temporary address, lost on power cycle) and discovery
        // re-runs so the connection uses the forced address.
        std::string resolveMacTarget(const DeviceConfig &device) {
            const std::string want = Common::StringUtil::NormalizeMac(device.mac);
            bool force_ip_sent = false;
            while (true) {
                const auto devices = discoverDevices(kMacDiscoveryTimeout);
                const DiscoveredDevice *match = nullptr;
                for (const auto &d: devices) {
                    if (Common::StringUtil::NormalizeMac(d.mac) == want) {
                        match = &d;
                        break;
                    }
                }
                if (match == nullptr) {
                    for (const auto &d: devices) {
                        g_log.warn("[eBUS] Discovered (no match): {} mac={} ip={}", d.display_id, d.mac, d.ip);
                    }
                    throw TransportError("[eBUS] No device with mac '" + device.mac + "' found (" +
                                         std::to_string(devices.size()) + " device(s) discovered)");
                }
                if (!match->configuration_valid) {
                    if (device.force_ip.enabled && !force_ip_sent) {
                        g_log.warn("[eBUS] Device {} is on a wrong subnet (ip={}); sending FORCEIP ip={} mask={} gw={} "
                                   "(temporary, lost on power cycle)",
                                   device.mac, match->ip, device.force_ip.ip, device.force_ip.subnet_mask,
                                   device.force_ip.gateway);
                        const PvResult result = PvDeviceGEV::SetIPConfiguration(
                            PvString(device.mac.c_str()), PvString(device.force_ip.ip.c_str()),
                            PvString(device.force_ip.subnet_mask.c_str()), PvString(device.force_ip.gateway.c_str()));
                        if (!result.IsOK()) {
                            throw TransportError("[eBUS] FORCEIP to '" + device.mac + "' failed: " +
                                                 pv(result.GetCodeString()));
                        }
                        force_ip_sent = true;
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        continue; // re-discover with the forced address
                    }
                    throw TransportError("[eBUS] Device " + device.mac + " has an invalid IP configuration for its NIC "
                                         "(device ip " + match->ip +
                                         "); fix the camera/NIC addressing or enable device.force_ip");
                }
                g_log.info("[eBUS] Discovered {} mac={} ip={}", match->display_id, match->mac, match->ip);
                return match->connection_id;
            }
        }
    } // namespace


    StreamReceiver::StreamReceiver(const NetworkConfig &network, Counters &counters)
        : network_(network), counters_(counters) {
    }


    StreamReceiver::~StreamReceiver() {
        try {
            stop();
            disconnect();
        } catch (...) {
        }
    }


    void StreamReceiver::connect(const DeviceConfig &device) {
        if (device_ != nullptr) {
            throw TransportError("[eBUS] Already connected");
        }
        std::string target;
        if (!device.id.empty()) {
            target = device.id;
        } else if (!device.mac.empty()) {
            target = resolveMacTarget(device);
        } else {
            target = device.ip;
        }
        if (target.empty()) {
            throw TransportError("[eBUS] No device target: set device.id, device.mac or device.ip");
        }
        g_log.info("[eBUS] Connecting to {} ...", target);

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
            disconnect();
            throw TransportError("[eBUS] '" + target + "' is not a GigE Vision device");
        }
        device_->RegisterEventSink(this);
        connection_target_ = target;
        g_log.info("[eBUS] Connected");
    }


    void StreamReceiver::openStream() {
        if (device_gev_ == nullptr) {
            throw TransportError("[eBUS] OpenStream: not connected");
        }
        if (stream_ != nullptr) {
            throw TransportError("[eBUS] Stream already open");
        }

        PvResult result;
        stream_ = PvStream::CreateAndOpen(PvString(connection_target_.c_str()), &result);
        if (stream_ == nullptr || !result.IsOK()) {
            if (stream_ != nullptr) {
                PvStream::Free(stream_);
            }
            stream_ = nullptr;
            throw TransportError("[eBUS] Cannot open stream: " + pv(result.GetCodeString()));
        }
        stream_gev_ = dynamic_cast<PvStreamGEV *>(stream_);
        if (stream_gev_ == nullptr) {
            PvStream::Free(stream_);
            stream_ = nullptr;
            throw TransportError("[eBUS] Stream is not GigE Vision");
        }

        if (network_.packet_size > 0) {
            // Fixed packet size for a known network path.
            PvGenInteger *packet_size = dynamic_cast<PvGenInteger *>(device_->GetParameters()->
                Get("GevSCPSPacketSize"));
            if (packet_size == nullptr || !packet_size->SetValue(static_cast<std::int64_t>(network_.packet_size)).
                IsOK()) {
                g_log.warn("[eBUS] Cannot set GevSCPSPacketSize={}, falling back to negotiation", network_.packet_size);
                device_gev_->NegotiatePacketSize();
            }
        } else {
            const PvResult negotiated = device_gev_->NegotiatePacketSize();
            if (!negotiated.IsOK()) {
                g_log.warn("[eBUS] Packet size negotiation failed ({}), device default in effect",
                           pv(negotiated.GetCodeString()));
            }
        }

        const std::uint32_t rx_bytes = static_cast<std::uint32_t>(network_.socket_rx_buffer_mb) * 1024u * 1024u;
        const PvResult rx_result = stream_gev_->SetUserModeSocketRxBufferSize(rx_bytes);
        if (!rx_result.IsOK()) {
            g_log.warn("[eBUS] SetUserModeSocketRxBufferSize({} MB) failed: {} — the kernel caps socket "
                       "buffers at net.core.rmem_max; run on the HOST: "
                       "sudo sysctl -w net.core.rmem_max={}",
                       network_.socket_rx_buffer_mb, pv(rx_result.GetCodeString()), rx_bytes);
        }

        const PvResult dest = device_gev_->SetStreamDestination(stream_gev_->GetLocalIPAddress(),
                                                                stream_gev_->GetLocalPort());
        if (!dest.IsOK()) {
            throw TransportError("[eBUS] SetStreamDestination failed: " + pv(dest.GetCodeString()));
        }
        g_log.trace("[eBUS] Stream open on {}:{}", pv(stream_gev_->GetLocalIPAddress()), stream_gev_->GetLocalPort());
    }


    void StreamReceiver::start(IFrameSink &sink, const ExpectedGeometry &expected,
                               double expected_fps, const WatchdogConfig &watchdog,
                               double no_frame_abort_s) {
        if (stream_ == nullptr) {
            throw TransportError("[eBUS] Start: stream not open");
        }
        if (streaming_.load()) {
            throw TransportError("[eBUS] Already streaming");
        }
        // Wire size per frame: packed formats carry fewer bytes than W*H*bpp
        pixel_info_ = pixelFormatInfo(expected.pixel_format);
        const std::size_t pixels = static_cast<std::size_t>(expected.width) * expected.height;
        const std::size_t wire_bytes =
                pixel_info_ != nullptr
                    ? wireBytes(*pixel_info_, pixels)
                    : pixels * expected.bytes_per_pixel;
        if (expected.payload_size != wire_bytes) {
            throw TransportError(
                "[eBUS] Expected geometry inconsistent: payload " + std::to_string(expected.payload_size) +
                " != " + std::to_string(wire_bytes) + " wire bytes for " + std::to_string(expected.width) +
                "x" + std::to_string(expected.height) + " " + expected.pixel_format +
                " (chunk data active, or geometry read before configuration?)");
        }
        if (pixel_info_ != nullptr && pixel_info_->packed) {
            unpack_buf_.resize(pixels); // reused every frame; sink sees canonical uint16
        }

        expected_ = expected;
        watchdog_ = watchdog;
        no_frame_abort_s_ = no_frame_abort_s;
        first_frame_checked_ = false;
        preamble_checked_ = false;
        tracker_ = BlockIdTracker();
        // Full state reset: this object supports reconnect cycles (stop -> disconnect -> connect -> start);
        // so stale link-loss/error state must not survive
        stop_requested_.store(false);
        failed_.store(false);
        link_lost_.store(false);
        acq_stop_sent_.store(false);
        frames_delivered_.store(0);
        queue_failures_ = 0;
        buffers_deferred_ = false;
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_message_.clear();
        }

        const std::uint32_t payload = device_->GetPayloadSize();
        if (payload != expected.payload_size) {
            // The expected geometry came from readGeometry() moments ago;
            // a mismatch means the camera state changed since — the first-frame check would abort anyway,
            // so fail fast before enabling the stream.
            throw TransportError("[eBUS] Device PayloadSize " + std::to_string(payload) +
                                 " != expected " + std::to_string(expected.payload_size) +
                                 " (camera state changed after configuration?)");
        }
        const BufferPoolPlan plan = planBufferPool(network_, expected_fps, payload, stream_->GetQueuedBufferMaximum());
        if (plan.count == 0) {
            throw TransportError("[eBUS] Buffer pool plan failed (payload size 0?)");
        }
        if (plan.clamped_by_memory || plan.clamped_by_stream) {
            g_log.warn("[eBUS] Buffer pool clamped to {} buffers ({:.1f} MiB): stall absorption {:.2f} s", plan.count,
                       plan.bytes / 1048576.0, plan.achievable_stall_s);
        } else {
            g_log.trace("[eBUS] Buffer pool: {} buffers ({:.1f} MiB), stall absorption {:.2f} s", plan.count,
                        plan.bytes / 1048576.0, plan.achievable_stall_s);
        }

        buffers_.reserve(plan.count);
        for (std::uint32_t i = 0; i < plan.count; ++i) {
            auto buffer = std::make_unique<PvBuffer>();
            const PvResult alloc = buffer->Alloc(payload);
            if (!alloc.IsOK()) {
                freeBuffers_();
                throw TransportError(
                    "[eBUS] Buffer allocation failed at " + std::to_string(i) + "/" + std::to_string(plan.count) + ": "
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
                drainAborted_();
                freeBuffers_();
                throw TransportError("[eBUS] Initial QueueBuffer failed: " + pv(queued.GetCodeString()));
            }
        }

        PvResult result = device_->StreamEnable();
        if (!result.IsOK()) {
            if (drainAborted_()) {
                freeBuffers_();
            }
            throw TransportError("[eBUS] StreamEnable failed: " + pv(result.GetCodeString()));
        }
        PvGenCommand *start_command = dynamic_cast<PvGenCommand *>(device_->GetParameters()->Get("AcquisitionStart"));
        if (start_command == nullptr || !start_command->Execute().IsOK()) {
            device_->StreamDisable();
            if (drainAborted_()) {
                freeBuffers_();
            }
            throw TransportError("[eBUS] AcquisitionStart failed");
        }

        try {
            acquisition_thread_ = std::thread(&StreamReceiver::acquisitionLoop_, this, std::ref(sink));
        } catch (...) {
            // Unwind like the AcquisitionStart failure path
            // Never leave the camera acquiring with nobody retrieving
            executeAcquisitionStop_();
            device_->StreamDisable();
            if (drainAborted_()) {
                freeBuffers_();
            }
            throw;
        }
        streaming_.store(true);
        g_log.info("[eBUS] Acquisition started");
    }


    void StreamReceiver::executeAcquisitionStop_() {
        if (acq_stop_sent_.exchange(true)) {
            return; // once per run
        }
        PvGenCommand *stop_command = dynamic_cast<PvGenCommand *>(device_->GetParameters()->Get("AcquisitionStop"));
        if (stop_command == nullptr || !stop_command->Execute().IsOK()) {
            g_log.warn("[eBUS] AcquisitionStop failed (continuing teardown)");
        }
    }


    void StreamReceiver::requeue_(PvBuffer *buffer) {
        const PvResult result = stream_->QueueBuffer(buffer);
        if (result.IsOK() || result.GetCode() == PvResult::Code::PENDING) {
            return;
        }
        // A buffer that cannot re-enter the ring permanently shrinks the pool.
        ++queue_failures_;
        ++counters_.op_errors;
        g_log.error("[eBUS] QueueBuffer failed ({}), pool shrank by {} buffer(s)", pv(result.GetCodeString()),
                    queue_failures_);
        if (queue_failures_ >= buffers_.size()) {
            latchFatal_("buffer pool exhausted: every QueueBuffer failed");
        }
    }


    void StreamReceiver::acquisitionLoop_(IFrameSink &sink) {
        using clock = std::chrono::steady_clock;
        auto last_frame = clock::now();
        auto next_warn = last_frame + std::chrono::duration_cast<clock::duration>(
                             std::chrono::duration<double>(watchdog_.no_frame_warn_s)
                         );

        bool draining = false;
        auto silence_start = clock::now();
        auto drain_deadline = clock::now();

        while (true) {
            const bool fatal = failed_.load() || link_lost_.load();
            if (fatal) {
                // A fatal condition ends the loop immediately, drain included;
                // the AcquisitionStop for the live-link fatal cases is issued in stop()
                break;
            }
            if (!draining && sink.failed()) {
                latchFatal_("frame sink failed (see recorder error above)", FatalKind::kSinkFailed);
                break;
            }
            if (!draining && stop_requested_.load()) {
                executeAcquisitionStop_();
                draining = true;
                silence_start = clock::now();
                drain_deadline = clock::now() + kMaxDrainTime;
                g_log.trace("[eBUS] Acquisition stop requested; Draining in-flight frames");
            }
            if (draining && clock::now() >= drain_deadline) {
                g_log.warn(
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
                    g_log.warn("[eBUS] RetrieveBuffer error: {}", pv(result.GetCodeString()));
                    continue;
                }
                ++counters_.retrieve_timeouts;
                const double quiet = std::chrono::duration<double>(clock::now() - last_frame).count();
                if (no_frame_abort_s_ > 0.0 && quiet > no_frame_abort_s_) {
                    latchFatal_("watchdog: no frames for " + std::to_string(quiet) + " s", FatalKind::kWatchdog);
                    continue;
                }
                if (watchdog_.no_frame_warn_s > 0.0 && clock::now() >= next_warn) {
                    g_log.warn(
                        "[eBUS] No frames for {:.1f} s (external trigger idle, pulses stopped, or stream problem)",
                        quiet);
                    next_warn = clock::now() + std::chrono::duration_cast<clock::duration>(
                                    std::chrono::duration<double>(std::max(watchdog_.no_frame_warn_s, quiet))
                                );
                }
                continue;
            }

            // We hold a buffer from here: it MUST be re-queued on every path.
            if (!op_result.IsOK()) {
                ++counters_.op_errors;
                g_log.warn("[eBUS] Buffer operation error: {}", pv(op_result.GetCodeString()));
                requeue_(buffer);
                continue;
            }
            if (buffer->GetPayloadType() != PvPayloadTypeImage) {
                ++counters_.op_errors;
                g_log.warn("[eBUS] Non-image payload type {} dropped", static_cast<int>(buffer->GetPayloadType()));
                requeue_(buffer);
                continue;
            }
            if (!first_frame_checked_) {
                if (!checkFirstFrame_(*buffer)) {
                    requeue_(buffer);
                    continue; // latchFatal_ set; loop head exits
                }
                first_frame_checked_ = true;
            }

            const auto observation = tracker_.observe(buffer->GetBlockID());
            if (observation.anomaly) {
                ++counters_.blockid_anomalies;
                g_log.warn("[eBUS] BlockID anomaly at {}", buffer->GetBlockID());
            }
            if (observation.gap_before > 0) {
                sink.onGap(observation.first_missing, observation.gap_before);
            }

            ++counters_.retrieve_ok;
            last_frame = clock::now();
            next_warn = last_frame + std::chrono::duration_cast<clock::duration>(
                            std::chrono::duration<double>(watchdog_.no_frame_warn_s));

            PvImage *image = buffer->GetImage();
            FrameView frame;
            if (pixel_info_ != nullptr && pixel_info_->packed) {
                // Unpack to the canonical right-justified uint16 layout
                const std::size_t n_px = static_cast<std::size_t>(expected_.width) * expected_.height;
                if (expected_.pixel_format == "Mono12Packed") {
                    unpackMono12Packed(buffer->GetDataPointer(), n_px, unpack_buf_.data());
                } else {
                    unpackMono10Packed(buffer->GetDataPointer(), n_px, unpack_buf_.data());
                }
                frame.data = reinterpret_cast<const std::uint8_t *>(unpack_buf_.data());
                frame.size = n_px * 2;
            } else {
                frame.data = buffer->GetDataPointer();
                frame.size = static_cast<std::size_t>(buffer->GetAcquiredSize());
            }
            frame.width = image != nullptr ? image->GetWidth() : 0;
            frame.height = image != nullptr ? image->GetHeight() : 0;
            frame.bytes_per_pixel = expected_.bytes_per_pixel;
            frame.block_id = buffer->GetBlockID();

            if (!preamble_checked_) {
                preamble_checked_ = true;
                warnIfUnexpectedStatusLine_(frame.data);
            }

            sink.onFrame(frame);
            frames_delivered_.fetch_add(1);
            requeue_(buffer);

            if (draining) {
                silence_start = clock::now(); // still receiving: extend the drain
            }
        }
        streaming_.store(false);
    }


    bool StreamReceiver::checkFirstFrame_(PvBuffer &buffer) {
        PvImage *image = buffer.GetImage();
        const std::uint32_t width = image != nullptr ? image->GetWidth() : 0;
        const std::uint32_t height = image != nullptr ? image->GetHeight() : 0;
        const std::uint64_t size = buffer.GetAcquiredSize();
        if (width != expected_.width || height != expected_.height || size != expected_.payload_size) {
            latchFatal_("first frame mismatch: got " + std::to_string(width) + "x" +
                        std::to_string(height) + " (" + std::to_string(size) + " B), expected " +
                        std::to_string(expected_.width) + "x" + std::to_string(expected_.height) + " (" +
                        std::to_string(expected_.payload_size) +
                        " B). Check binning/MROI/status_line/pixel_format vs the camera state.",
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
                latchFatal_("first frame pixel type mismatch: camera delivers type " +
                            std::to_string(static_cast<int>(actual)) + ", configured " +
                            expected_.pixel_format,
                            FatalKind::kFirstFrame);
                return false;
            }
        }

        g_log.trace("[eBUS] First frame OK: {}x{} ({} B), block_id {}", width, height, size, buffer.GetBlockID());
        return true;
    }


    void StreamReceiver::warnIfUnexpectedStatusLine_(const std::uint8_t *data) {
        // Status-line double-check on CANONICAL (unpacked) data: when the config
        // says the status line is OFF, the last row must not start with the
        // 0x66BB00FF preamble (encoded in the lower 8 bits of the first 4
        // pixels, LSB first). Runs once, after any unpack, so the byte layout
        // is identical for packed and unpacked wire formats.
        if (expected_.status_line || expected_.bytes_per_pixel != 2 || expected_.width < 4 ||
            expected_.height < 1 || data == nullptr) {
            return;
        }
        const std::size_t last_row =
                static_cast<std::size_t>(expected_.height - 1) * expected_.width * 2;
        if (data[last_row] == 0xFF && data[last_row + 2] == 0x00 &&
            data[last_row + 4] == 0xBB && data[last_row + 6] == 0x66) {
            g_log.warn("[eBUS] Status-line preamble detected in the last row but the configuration "
                "says status_line=off — the last band row may contain metadata, "
                "check EnStatusLine on the camera");
        }
    }


    void StreamReceiver::stop() {
        if (!streaming_.load() && !acquisition_thread_.joinable()) {
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
            executeAcquisitionStop_();
        }
        if (device_ != nullptr) {
            device_->StreamDisable();
        }
        if (stream_gev_ != nullptr) {
            stream_gev_->FlushPacketQueue();
        }
        if (drainAborted_()) {
            freeBuffers_();
        } else {
            // The stream may still reference buffers; defer the free until after Close.
            buffers_deferred_ = true;
            g_log.warn("[eBUS] Stream queue not fully drained; buffer release deferred to disconnect()");
        }
        streaming_.store(false);
        g_log.info("[eBUS] Acquisition stopped ({} frames delivered)", frames_delivered_.load());
    }


    bool StreamReceiver::drainAborted_() {
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


    void StreamReceiver::freeBuffers_() { buffers_.clear(); }


    void StreamReceiver::disconnect() {
        if (streaming_.load()) stop();
        if (stream_ != nullptr) {
            stream_->Close();
            PvStream::Free(stream_);
            stream_ = nullptr;
            stream_gev_ = nullptr;
        }
        freeBuffers_(); // safe now: the stream (if any) is closed
        buffers_deferred_ = false;
        if (device_ != nullptr) {
            device_->UnregisterEventSink(this);
            device_->Disconnect();
            PvDevice::Free(device_);
            device_ = nullptr;
            device_gev_ = nullptr;
        }
    }


    void StreamReceiver::latchFatal_(const std::string &message, FatalKind kind) {
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            if (failed_.load()) return;
            error_message_ = message;
        }
        fatal_kind_.store(kind);
        failed_.store(true);
        g_log.error("[eBUS] Transport fatal: {}", message);
    }


    std::string StreamReceiver::errorMessage() const {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return error_message_;
    }


    void StreamReceiver::OnLinkDisconnected(PvDevice *) {
        link_lost_.store(true);
        latchFatal_("GigE link lost (camera disconnected or powered off)", FatalKind::kLinkLost);
    }
} // namespace fx10
