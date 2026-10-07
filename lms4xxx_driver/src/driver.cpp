#include "driver_internal.h"

#include "command_builder.h"
#include "error.h"
#include "logger.h"
#include "time_util.h"
#include "utility.h"

namespace {
    common::DriverLog g_log{"LMS4xxx"};
    // Resynchronisation failures are not a count of lost scans.
    constexpr std::uint64_t kMaxConsecutiveFramingErrors = 1000;
}

namespace lms4xxx {
    Lms4xxxDriver::Impl::Impl(const DriverConfig &cfg)
        : config(cfg), commands(cfg.network.response_timeout_ms, cfg.name.empty() ? cfg.device.ip : cfg.name) {
        if (config.name.empty()) config.name = config.device.ip;
    }

    void Lms4xxxDriver::Impl::ReportError(std::error_code ec, const std::string &detail) {
        ErrorCallback cb;
        {
            std::lock_guard lock(callback_mutex);
            cb = error_callback;
        }
        if (cb) {
            try {
                cb(ec, detail);
            } catch (const std::exception &e) {
                g_log.Warn("[{}] Error callback threw: {}", Tag(), e.what());
            }
        }
    }


    std::error_code Lms4xxxDriver::Impl::ParkMeasurement(bool streaming, bool reconnect) {
        const bool pending = measurement_shutdown.Pending();
        bool connection_prepared = false;
        const auto ec = measurement_shutdown.Park([&](std::string_view name, int expected_status) {
            if (reconnect && !connection_prepared) {
                if (tcp_client) tcp_client->Disconnect();
                tcp_client = std::make_unique<TcpClient>(MakeTcpOptions(config.device, config.network));
                if (const auto connect_ec = tcp_client->Connect(config.network.connect_timeout_ms)) {
                    return connect_ec;
                }
                commands.Reset();
                connection_prepared = true;
            }
            const auto frame = name == "SetAccessMode" ? CommandBuilder::BuildLogin()
                                                       : CommandBuilder::BuildStandby();
            if (streaming) {
                constexpr int kShutdownMethodTimeoutMs = 1000;
                return CallMethodStreaming(frame, std::string(name), expected_status, kShutdownMethodTimeoutMs)
                    ? std::error_code{} : make_error_code(ErrorCode::kStandbyUnconfirmed);
            }
            return commands.CallMethod(tcp_client.get(), frame, name, static_cast<std::uint8_t>(expected_status));
        });
        if (pending) {
            if (measurement_shutdown.Status().standby_confirmed) {
                g_log.Info("[{}] Laser parked (standby confirmed); the motor keeps turning", Tag());
            } else {
                g_log.Warn("[{}] Device standby was not confirmed ({}); the laser may still be on", Tag(),
                           ec.message());
            }
        }
        return ec;
    }


    Lms4xxxDriver::Lms4xxxDriver(const DriverConfig &config) : impl_(std::make_unique<Impl>(config)) {
    }


    Lms4xxxDriver::~Lms4xxxDriver() {
        Disconnect();
    }


    std::error_code Lms4xxxDriver::Connect() {
        impl_->SetState(ConnectionState::kConnecting);
        g_log.Info("[{}] Connecting to {}:{}", impl_->Tag(), impl_->config.device.ip, impl_->config.device.port);

        impl_->tcp_client = std::make_unique<TcpClient>(MakeTcpOptions(impl_->config.device, impl_->config.network));

        auto ec = impl_->tcp_client->Connect(impl_->config.network.connect_timeout_ms);
        if (ec) {
            impl_->SetState(ConnectionState::kError);
            impl_->ReportError(ec, "TCP connect failed");
            return ec;
        }

        impl_->SetState(ConnectionState::kConnected);
        impl_->commands.Reset();
        g_log.Info("[{}] Connected", impl_->Tag());
        return {};
    }


    std::error_code Lms4xxxDriver::StartScanning() {
        if (!impl_->tcp_client || !impl_->tcp_client->IsConnected()) {
            StopScanning();
            return make_error_code(ErrorCode::kNotConnected);
        }
        if (impl_->scanning.load(std::memory_order_relaxed)) {
            return make_error_code(ErrorCode::kAlreadyScanning);
        }

        impl_->stats.Reset(); // ntp_status keeps Configure()'s probe result
        impl_->fault.store(false, std::memory_order_release);
        impl_->time_fault.store(false, std::memory_order_release);
        impl_->stop_stream_requested.store(false, std::memory_order_release);
        impl_->device_warnings_monotonic_us = 0;
        impl_->device_no_ntp = false;

        impl_->ring_buffer = std::make_unique<FrameRingBuffer>(impl_->config.network.ring_buffer_frames);

        auto *stats_ptr = &impl_->stats;
        auto *ring_ptr = impl_->ring_buffer.get();

        impl_->frame_receiver = std::make_unique<FrameReceiver>(
            [stats_ptr, ring_ptr](RawFrame &&frame) {
                stats_ptr->frames_received.fetch_add(1, std::memory_order_relaxed);
                if (!ring_ptr->try_push(std::move(frame))) {
                    stats_ptr->frames_dropped.fetch_add(1, std::memory_order_relaxed);
                }
            },
            [stats_ptr, impl = impl_.get()](FrameReceiver::FrameError error, std::uint64_t consecutive) {
                if (error == FrameReceiver::FrameError::kChecksumMismatch) {
                    stats_ptr->crc_errors.fetch_add(1, std::memory_order_relaxed);
                } else {
                    stats_ptr->framing_errors.fetch_add(1, std::memory_order_relaxed);
                }
                // Resync examines each possible STX, so a stream that is no
                // longer CoLa B produces errors indefinitely instead of ever
                // failing. Past this many in a row without a single good frame
                // the link is not going to recover on its own.
                if (consecutive >= kMaxConsecutiveFramingErrors &&
                    !impl->fault.exchange(true, std::memory_order_acq_rel)) {
                    g_log.Error("[{}] {} consecutive framing errors without a valid frame — the stream is not "
                                "CoLa B any more; stopping", impl->Tag(), consecutive);
                    impl->ReportError(make_error_code(ErrorCode::kProtocolError),
                                       "stream desynchronised beyond recovery");
                }
            },
            CommandChannel::kMaxFrameBytes, impl_->Tag());

        {
            auto frame = CommandBuilder::BuildStartStream();
            ColaBMessage response;
            impl_->stream_start_requested = true;
            auto ec = impl_->commands.SendAndReceive(impl_->tcp_client.get(), frame, response, impl_->config.network.response_timeout_ms);
            if (ec) {
                g_log.Error("[{}] Start stream command failed: {}", impl_->Tag(), ec.message());
                StopScanning();
                return ec;
            }
            auto validate_ec = impl_->commands.ValidateResponse(response, CommandType::kEventAnswer, "LMDscandata");
            if (validate_ec) {
                StopScanning();
                return validate_ec;
            }

            if (response.payload.empty() || response.payload[0] != 0x01) {
                g_log.Error("[{}] Start stream rejected", impl_->Tag());
                StopScanning();
                return make_error_code(ErrorCode::kCommandRejected);
            }
        }

        impl_->scanning.store(true, std::memory_order_release);
        impl_->receive_running.store(true, std::memory_order_release);
        impl_->parse_running.store(true, std::memory_order_release);

        try {
            impl_->parse_thread = std::thread([this]() { impl_->ParseLoop(); });
            impl_->receive_thread = std::thread([this]() { impl_->ReceiveLoop(); });
            if (impl_->config.ntp.enabled) {
                impl_->ntp_watch_running.store(true, std::memory_order_release);
                impl_->ntp_watch_thread = std::thread([this]() { impl_->NtpWatchLoop(); });
            }
        } catch (const std::system_error &e) {
            StopScanning(); // includes partially constructed worker sets
            return e.code();
        } catch (...) {
            StopScanning();
            throw;
        }

        impl_->ConfigureReceiveThread();

        impl_->SetState(ConnectionState::kScanning);
        g_log.Trace("[{}] Scanning started (ring buffer capacity: {} frames)", impl_->Tag(),
                    impl_->ring_buffer->capacity());
        return {};
    }


    std::error_code Lms4xxxDriver::StopScanning() {
        const bool have_workers = impl_->receive_thread.joinable() || impl_->parse_thread.joinable() ||
                                  impl_->ntp_watch_thread.joinable();
        if (!have_workers && !impl_->scanning.load(std::memory_order_relaxed)) {
            // Configure succeeded but the owner cancelled or its writer failed,
            // or StartStream's reply was lost before the workers were constructed.
            return impl_->ParkMeasurement(false, impl_->stream_start_requested ||
                                          impl_->commands.Uncertain() || !impl_->tcp_client ||
                                          !impl_->tcp_client->IsConnected());
        }

        g_log.Trace("[{}] Stopping scan...", impl_->Tag());

        // Preserve the normal drain path: responses are decoded by the existing
        // workers until standby has been acknowledged or its bounded wait ends.
        impl_->stop_stream_requested.store(true, std::memory_order_release);
        if (impl_->tcp_client && impl_->tcp_client->IsConnected()) {
            if (const auto ec = impl_->tcp_client->Write(CommandBuilder::BuildStopStream())) {
                g_log.Warn("[{}] Failed to send stop stream command (non-fatal): {}", impl_->Tag(), ec.message());
            }
            if (impl_->receive_thread.joinable() && impl_->parse_thread.joinable() &&
                impl_->parse_running.load(std::memory_order_acquire) &&
                (!impl_->fault.load(std::memory_order_acquire) || impl_->time_fault.load(std::memory_order_acquire))) {
                impl_->ParkMeasurement(true, false);
            }
        }

        impl_->receive_running.store(false, std::memory_order_release);
        impl_->scanning.store(false, std::memory_order_release);

        // SHUT_RD wakes the bounded receive wait; the loop then sees the stop flag.
        if (impl_->tcp_client) {
            impl_->tcp_client->ShutdownReceive();
        }

        impl_->ntp_watch_running.store(false, std::memory_order_release);
        if (impl_->ntp_watch_thread.joinable()) {
            impl_->ntp_watch_thread.join();
        }
        if (impl_->receive_thread.joinable()) {
            impl_->receive_thread.join();
        }
        impl_->parse_running.store(false, std::memory_order_release);
        if (impl_->parse_thread.joinable()) {
            impl_->parse_thread.join();
        }
        RawFrame remaining;
        std::uint64_t discarded = 0;
        while (impl_->ring_buffer && impl_->ring_buffer->try_pop(remaining)) {
            ++discarded;
        }
        if (discarded > 0) {
            impl_->stats.frames_dropped.fetch_add(discarded, std::memory_order_relaxed);
            g_log.Warn("[{}] {} raw frames discarded after a parse fault", impl_->Tag(), discarded);
        }

        // A failed parser or incomplete worker set cannot receive a command
        // answer. Only after joining every worker may cleanup replace the socket.
        const auto cleanup_ec = impl_->ParkMeasurement(false, true);
        impl_->SetState(ConnectionState::kConnected);
        g_log.Trace("[{}] Scanning stopped", impl_->Tag());
        return cleanup_ec;
    }


    std::error_code Lms4xxxDriver::Disconnect() {
        const auto cleanup_ec = StopScanning();

        if (impl_->tcp_client) {
            impl_->tcp_client->Disconnect();
        }

        impl_->frame_receiver.reset();
        impl_->ring_buffer.reset();

        impl_->SetState(ConnectionState::kDisconnected);
        g_log.Trace("[{}] Disconnected", impl_->Tag());
        return cleanup_ec;
    }


    MeasurementShutdownStatus Lms4xxxDriver::GetMeasurementShutdownStatus() const {
        return impl_->measurement_shutdown.Status();
    }


    void Lms4xxxDriver::SetScanCallback(ScanDataCallback callback) {
        std::lock_guard lock(impl_->callback_mutex);
        impl_->scan_callback = std::move(callback);
    }


    void Lms4xxxDriver::SetErrorCallback(ErrorCallback callback) {
        std::lock_guard lock(impl_->callback_mutex);
        impl_->error_callback = std::move(callback);
    }



    bool Lms4xxxDriver::IsScanning() const {
        return impl_->scanning.load(std::memory_order_acquire);
    }


    bool Lms4xxxDriver::HasFault() const {
        return impl_->fault.load(std::memory_order_acquire);
    }


    DriverStatistics::Snapshot Lms4xxxDriver::GetStatistics() const {
        return impl_->stats.GetSnapshot();
    }


    const DeviceIdentity &Lms4xxxDriver::GetDeviceIdentity() const {
        return impl_->identity;
    }



    std::uint64_t Lms4xxxDriver::MicrosSinceLastFrame() const {
        const auto last = impl_->stats.last_frame_time_us.load(std::memory_order_relaxed);
        if (last == 0) {
            return 0; // nothing has arrived yet; the caller decides what that means
        }
        // FrameReceiver stamps frames with the STEADY clock (a wall-clock step
        // must not be able to fake or mask a stall), so compare against the
        // same one.
        const auto now = common::TimeUtil::SteadyNowUs();
        return now > last ? now - last : 0;
    }


    const DeviceAudit &Lms4xxxDriver::GetDeviceAudit() const {
        return impl_->audit;
    }


    void Lms4xxxDriver::RequestTelemetry() {
        impl_->RequestTelemetry();
    }


    bool Lms4xxxDriver::TakeTelemetry(TelemetrySample &out) {
        return impl_->TakeTelemetry(out);
    }
} // namespace lms4xxx
