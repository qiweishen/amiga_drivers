#include <cstring>

#include "driver_internal.h"
#include "command_builder.h"
#include "error.h"
#include "scan_data_parser.h"
#include "ntp_probe.h"
#include "scan_time.h"
#include "scan_verify.h"
#include "logger.h"
#include "thread_util.h"
#include "time_util.h"


namespace {
    common::DriverLog g_log{"LMS4xxx"};
    constexpr std::size_t kReadBufferSize = 16 * 1024;
    constexpr int kNtpProbeTimeoutMs = 2000;
    constexpr int kNtpProbeMaxFailures = 3;

    // Telegram time stamp block as ISO-8601 UTC for log lines
    std::string FormatDeviceTime(const lms4xxx::ScanData &scan) {
        if (!scan.has_timestamp) {
            return "<no time stamp block>";
        }
        const auto &ts = scan.timestamp;
        return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z", ts.year, ts.month, ts.day, ts.hour,
                           ts.minute, ts.second, ts.microsecond);
    }

} // namespace

namespace lms4xxx {
    void Lms4xxxDriver::Impl::RequestTelemetry() {
        if (!tcp_client || !tcp_client->IsConnected()) {
            return;
        }
        for (const char *name: {"OPcurtmpdev", "SCdevicestate", "EMActiveCustomerInfo"}) {
            const auto frame = ColaBCodec::Encode(CommandType::kReadByName, name);
            if (const auto ec = tcp_client->Write(frame)) {
                g_log.Debug("[{}] Telemetry request {} failed: {}", Tag(), name, ec.message());
                return; // the link is in trouble; the watchdog will deal with it
            }
        }
    }


    void Lms4xxxDriver::Impl::HandleNonScanFrame(const ColaBMessage &msg) {
        if (msg.command_type == CommandType::kEventAnswer && msg.command_name == "LMDscandata") {
            const std::uint8_t expected = stop_stream_requested.load(std::memory_order_acquire) ? 0 : 1;
            if (msg.payload.size() == 1 && msg.payload[0] == expected) {
                g_log.Trace("[{}] LMDscandata subscription acknowledgement: {}", Tag(), expected);
            } else {
                stats.unexpected_replies.fetch_add(1, std::memory_order_relaxed);
                g_log.Warn("[{}] Unexpected LMDscandata subscription acknowledgement (expected {})", Tag(), expected);
            }
            return;
        }
        if (msg.command_type == CommandType::kMethodAnswer) {
            std::lock_guard lock(telemetry_mutex);
            last_method_name = msg.command_name;
            last_method_status = msg.payload.empty() ? -1 : static_cast<int>(msg.payload[0]);
            return;
        }
        if (msg.command_type == CommandType::kErrorAnswer) {
            const std::uint16_t code = msg.payload.size() >= 2
                                           ? ColaBCodec::DecodeUint16(msg.payload.data())
                                           : 0xFFFF;
            g_log.Warn("[{}] Device answered sFA {} ({}) mid-stream", Tag(), code, SopasErrorText(code));
            std::lock_guard lock(telemetry_mutex);
            last_method_name = "sFA";
            last_method_status = -1;
            return;
        }
        if (msg.command_type != CommandType::kReadAnswer) {
            stats.unexpected_replies.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::lock_guard lock(telemetry_mutex);
        if (msg.command_name == "OPcurtmpdev" && msg.payload.size() >= 4) {
            telemetry_latest.temperature_c = ColaBCodec::DecodeFloat(msg.payload.data());
        } else if (msg.command_name == "SCdevicestate" && !msg.payload.empty()) {
            telemetry_latest.device_state = msg.payload[0];
        } else if (msg.command_name == "EMActiveCustomerInfo") {
            bool complete = false;
            telemetry_latest.warnings = DecodeActiveMessages(msg.payload, &complete);
            device_warnings_monotonic_us = complete ? common::TimeUtil::SteadyNowUs() : 0;
            if (complete) {
                const bool no_ntp = std::any_of(telemetry_latest.warnings.begin(), telemetry_latest.warnings.end(),
                    [](const std::string &warning) { return warning.starts_with("38:"); });
                if (config.ntp.enabled && no_ntp && !device_no_ntp) {
                    stats.device_no_ntp_events.fetch_add(1, std::memory_order_relaxed);
                    g_log.Warn("[{}] Device reports No NTP signal; host server reachability does not prove device lock", Tag());
                }
                device_no_ntp = no_ntp;
                if (config.ntp.enabled && no_ntp)
                    stats.ntp_status.store(DriverStatistics::NtpStatus::kNoSignal, std::memory_order_relaxed);
            } else {
                g_log.Warn("[{}] Incomplete device warning readback; NTP warning status unknown", Tag());
            }
        } else {
            stats.unexpected_replies.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        telemetry_latest.unix_us = common::TimeUtil::RealtimeNowUs();
        telemetry_dirty = true;
    }


    bool Lms4xxxDriver::Impl::CallMethodStreaming(const std::vector<std::uint8_t> &frame, const std::string &name,
                               int expected_status, int timeout_ms) {
        if (!tcp_client || !tcp_client->IsConnected()) {
            return false;
        }
        {
            std::lock_guard lock(telemetry_mutex);
            last_method_name.clear();
            last_method_status = -1;
        }
        if (const auto ec = tcp_client->Write(frame)) {
            g_log.Warn("[{}] {} could not be sent: {}", Tag(), name, ec.message());
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            {
                std::lock_guard lock(telemetry_mutex);
                if (last_method_name == name) {
                    if (last_method_status == expected_status) {
                        return true;
                    }
                    g_log.Warn("[{}] {} refused by the device (status {}, expected {})", Tag(), name,
                               last_method_status, expected_status);
                    return false;
                }
                if (last_method_name == "sFA") {
                    return false; // already logged with the SOPAS code
                }
            }
        }
        g_log.Warn("[{}] {} was not answered within {} ms", Tag(), name, timeout_ms);
        return false;
    }


    bool Lms4xxxDriver::Impl::TakeTelemetry(TelemetrySample &out) {
        std::lock_guard lock(telemetry_mutex);
        if (!telemetry_dirty) {
            return false;
        }
        out = telemetry_latest;
        telemetry_dirty = false;
        return true;
    }


    void Lms4xxxDriver::Impl::ConfigureReceiveThread() {
        if (config.network.receive_thread_priority > 0) {
            const int prio = config.network.receive_thread_priority;
            if (const int ret = common::ThreadUtil::SetRealtimePriority(receive_thread, prio); ret != 0) {
                g_log.Warn("[{}] Failed to set SCHED_FIFO priority {}: {} (requires root or CAP_SYS_NICE)", Tag(),
                           prio, std::strerror(ret));
            } else {
                g_log.Trace("[{}] Receive thread: SCHED_FIFO priority {}", Tag(), prio);
            }
        }

        if (config.network.receive_thread_cpu >= 0) {
            const int cpu = config.network.receive_thread_cpu;
            if (const int ret = common::ThreadUtil::PinToCpu(receive_thread, cpu); ret != 0) {
                g_log.Warn("[{}] Failed to set CPU affinity to core {}: {}", Tag(), cpu, std::strerror(ret));
            } else {
                g_log.Trace("[{}] Receive thread pinned to CPU {}", Tag(), cpu);
            }
        }
    }


    void Lms4xxxDriver::Impl::ReceiveLoop() {
        g_log.Trace("[{}] Receive thread started", Tag());
        try {
            std::vector<std::uint8_t> buf(kReadBufferSize);
            while (receive_running.load(std::memory_order_acquire)) {
                std::error_code ec;
                const auto n = tcp_client->ReadSome(buf.data(), buf.size(), ec);
                if (n > 0) {
                    stats.bytes_received.fetch_add(n, std::memory_order_relaxed);
                    frame_receiver->Feed(buf.data(), n);
                }
                if (ec) {
                    if (receive_running.load(std::memory_order_relaxed)) {
                        g_log.Error("[{}] Receive thread: read error: {}", Tag(), ec.message());
                        fault.store(true, std::memory_order_release);
                        ReportError(ec, "receive thread read error");
                        SetState(ConnectionState::kError);
                    }
                    break;
                }
            }
        } catch (const std::exception &e) {
            fault.store(true, std::memory_order_release);
            ReportError(make_error_code(ErrorCode::kProtocolError), std::string("receive worker failed: ") + e.what());
            SetState(ConnectionState::kError);
        } catch (...) {
            fault.store(true, std::memory_order_release);
            ReportError(make_error_code(ErrorCode::kProtocolError), "receive worker failed with an unknown exception");
            SetState(ConnectionState::kError);
        }
        receive_running.store(false, std::memory_order_release);
        g_log.Trace("[{}] Receive thread stopped", Tag());
    }


    void Lms4xxxDriver::Impl::VerifyFirstScan(const ScanData &scan) {
        const std::string problems = lms4xxx::VerifyScanContent(scan, config.scan, config.ntp.enabled);
        if (!problems.empty()) {
            g_log.Error("[{}] First scan does not match the configuration: {}", Tag(), problems);
            fault.store(true, std::memory_order_release);
            ReportError(make_error_code(ErrorCode::kInvalidConfig), "scan content mismatch: " + problems);
            return;
        }

        if (scan.scan_frequency != ScanFixed::kScanFrequencyCentiHz) {
            g_log.Warn("[{}] Scan frequency {} Hz (expected {})", Tag(), scan.scan_frequency / 100.0,
                       ScanFixed::kScanFrequencyCentiHz / 100);
        }
        if (scan.device_info.device_status_1 != DeviceStatus::kOk || scan.device_info.device_status_2 !=
            DeviceStatus::kOk) {
            g_log.Warn("[{}] Device status {}/{}", Tag(), static_cast<int>(scan.device_info.device_status_1),
                       static_cast<int>(scan.device_info.device_status_2));
        }
        g_log.Info("[{}] First scan verified: DIST1+{}+ANGL1+QLTY1, {} pts @ {:.4f} deg from {:.1f} deg", Tag(),
                   RemissionChannel(config.scan.remission), ScanFixed::kPointsPerScan,
                   ScanFixed::kAngularResolutionDeg, ScanFixed::kStartAngleDeg);
    }


    bool Lms4xxxDriver::Impl::ProbeNtpServer(std::string &detail) const {
        return Ntp::ProbeServer(config.ntp.server, kNtpProbeTimeoutMs, detail);
    }


    void Lms4xxxDriver::Impl::NtpWatchLoop() {
        g_log.Trace("[{}] NTP watch thread started (period {} s)", Tag(), config.ntp.check_status_s);
        int failures = 0;
        while (ntp_watch_running.load(std::memory_order_acquire)) {
            // Retry immediately after a miss so three misses resolve in seconds
            if (failures == 0) {
                const auto next = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(config.ntp.check_status_s);
                while (std::chrono::steady_clock::now() < next) {
                    if (!ntp_watch_running.load(std::memory_order_acquire)) {
                        g_log.Trace("[{}] NTP watch thread stopped", Tag());
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
            if (!ntp_watch_running.load(std::memory_order_acquire)) {
                break;
            }
            std::string detail;
            const bool reachable = ProbeNtpServer(detail);
            // The probe may finish after StopScanning() has requested a
            // stop. Its late result must not create a new shutdown fault.
            if (!ntp_watch_running.load(std::memory_order_acquire)) {
                break;
            }
            if (reachable) {
                stats.ntp_server_reachable.store(true, std::memory_order_relaxed);
                if (failures > 0) {
                    g_log.Info("[{}] NTP server {} reachable again ({})", Tag(), config.ntp.server, detail);
                }
                failures = 0;
                // A healthy host probe cannot certify the device's NTP state.
                continue;
            }
            ++failures;
            stats.ntp_server_reachable.store(false, std::memory_order_relaxed);
            if (failures < kNtpProbeMaxFailures) {
                g_log.Warn("[{}] NTP server {} probe failed ({}/{}): {}", Tag(), config.ntp.server, failures,
                           kNtpProbeMaxFailures, detail);
                continue; // immediate retry
            }
            stats.ntp_status.store(DriverStatistics::NtpStatus::kUnreachable, std::memory_order_relaxed);
            g_log.Error("[{}] NTP server {} unreachable from this host ({} consecutive probes failed: {}); "
                        "device synchronization cannot be inferred",
                        Tag(), config.ntp.server, failures, detail);
            time_fault.store(true, std::memory_order_release);
            fault.store(true, std::memory_order_release);
            ReportError(make_error_code(ErrorCode::kInvalidConfig), "NTP server unreachable: " + detail);
            return; // fault latched; the app terminates the run
        }
        g_log.Trace("[{}] NTP watch thread stopped", Tag());
    }


    void Lms4xxxDriver::Impl::AssessScanTime(ScanData &scan, TimeGate &gate) {
        const std::int64_t device_time_us = DeviceTimeUnixUs(scan.timestamp);
        const bool plausible = scan.has_timestamp && DeviceTimePlausible(scan.timestamp);
        auto &observation = scan.clock_observation;
        if (!config.ntp.enabled) {
            observation = {ClockFlag::kAssessed | ClockFlag::kNtpDisabled | ClockFlag::kAbsoluteTimeUnverified, 0};
            return;
        }
        observation = gate.tracker.Observe(device_time_us, plausible, scan.time_since_startup_us,
                                           config.ntp.max_time_step_ms);
        const auto now_us = common::TimeUtil::SteadyNowUs();
        const bool warnings_fresh = device_warnings_monotonic_us != 0 &&
            now_us >= device_warnings_monotonic_us && now_us - device_warnings_monotonic_us <= 30000000;
        if (!warnings_fresh) observation.flags |= ClockFlag::kDeviceNtpUnknown;
        else if (device_no_ntp) observation.flags |= ClockFlag::kDeviceNoNtp;

        const bool backward_utc = (observation.flags & ClockFlag::kBackwardUtc) != 0;
        if (backward_utc) {
            stats.utc_backwards.fetch_add(1, std::memory_order_relaxed);
        }
        const bool time_anomaly = IsTimeAnomaly(observation);
        if (time_anomaly) {
            stats.time_anomaly_events.fetch_add(1, std::memory_order_relaxed);
            gate.anomaly_seen = true;
        }
        if (ShouldLogClockChange(observation)) {
            // UTC regression and UTC/uptime disagreement are different
            // quantities. An uptime reset still has a measurable UTC offset,
            // but no interpretable clock step relative to uptime.
            const auto clock_step = (observation.flags & ClockFlag::kUptimeDiscontinuity)
                ? std::string("n/a") : std::to_string(observation.step_us);
            const char *event = backward_utc ? "Device UTC moved backwards" : "Device UTC/uptime disagreement";
            const char *direction = backward_utc || observation.step_us < 0 ? "backward" : "forward";
            g_log.Warn("[{}] {}: direction={}, utc_offset_us={} ({:.3f} ms), "
                       "uptime_delta_us={}, clock_step_us={}, previous_utc_us={}, current_utc_us={}, "
                       "scan_counter={}, utc_back={}, log_threshold_us={}, time_anomaly={}; raw scan order/time retained",
                       Tag(), event, direction, observation.utc_delta_us, observation.utc_delta_us / 1000.0,
                       observation.uptime_delta_us, clock_step,
                       device_time_us - observation.utc_delta_us, device_time_us, scan.scan_counter,
                       stats.utc_backwards.load(std::memory_order_relaxed), kClockStepLogThresholdUs, time_anomaly);
        }
        if (observation.flags & ClockFlag::kRepeatedUtc)
            stats.utc_repeated.fetch_add(1, std::memory_order_relaxed);
        const bool large_step = (observation.flags & (ClockFlag::kStepExceeded | ClockFlag::kUptimeDiscontinuity)) != 0;
        if (large_step) {
            stats.clock_step_events.fetch_add(1, std::memory_order_relaxed);
        }
        const auto step_us = observation.step_us;
        const std::int64_t magnitude = step_us < 0 ? -step_us : step_us;
        std::int64_t seen = stats.max_time_step_us.load(std::memory_order_relaxed);
        while (magnitude > seen &&
               !stats.max_time_step_us.compare_exchange_weak(seen, magnitude, std::memory_order_relaxed)) {
        }
        auto status = !plausible ? DriverStatistics::NtpStatus::kNotLocked :
            (!warnings_fresh ? DriverStatistics::NtpStatus::kStale :
             (device_no_ntp ? DriverStatistics::NtpStatus::kNoSignal : DriverStatistics::NtpStatus::kUnverified));
        if (gate.anomaly_seen && status != DriverStatistics::NtpStatus::kNoSignal)
            status = DriverStatistics::NtpStatus::kClockAnomaly;
        if (!stats.ntp_server_reachable.load(std::memory_order_relaxed) && fault.load(std::memory_order_acquire))
            status = DriverStatistics::NtpStatus::kUnreachable;
        stats.ntp_status.store(status, std::memory_order_relaxed);

        const bool invalid_after_start = !plausible &&
            (gate.plausible_seen || std::chrono::steady_clock::now() >= gate.deadline);
        if ((invalid_after_start || large_step) && !time_fault.exchange(true, std::memory_order_acq_rel)) {
            g_log.Error("[{}] Device time fault (flags={}, step={} us, UTC={}); retaining scan and draining on stop",
                        Tag(), observation.flags, step_us, FormatDeviceTime(scan));
            fault.store(true, std::memory_order_release);
            ReportError(make_error_code(ErrorCode::kInvalidConfig), "device time fault; raw scans retained with quality flags");
        }
        if (plausible && !gate.plausible_seen) {
            gate.plausible_seen = true;
            g_log.Info("[{}] Device date plausible: {}; absolute time accuracy remains unverified", Tag(), FormatDeviceTime(scan));
        }
    }


    void Lms4xxxDriver::Impl::MonitorFrame(RawRecord &record) {
        // Corrupted bytes are still archived, but cannot drive command state or clocks.
        if (!record.frame.checksum_valid) return;
        const auto &bytes = record.frame.data;
        if (bytes.size() < 9) {
            stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto *data = bytes.data() + 8;
        const auto data_size = bytes.size() - 9;
        const std::string_view command(reinterpret_cast<const char *>(data), data_size);
        constexpr std::string_view scan_event = "sSN LMDscandata ";
        constexpr std::string_view scan_reply = "sRA LMDscandata ";
        const std::uint8_t *payload = nullptr;
        std::size_t payload_size = 0;
        ColaBMessage msg;
        if (command.starts_with(scan_event) || command.starts_with(scan_reply)) {
            // The common path borrows bytes from its owning RawFrame, without
            // a second payload copy or any per-point allocation.
            record.is_scan = true;
            payload = data + scan_event.size();
            payload_size = data_size - scan_event.size();
        } else {
            if (const auto ec = ColaBCodec::Decode(data, data_size, msg, Tag())) {
                stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
                g_log.Warn("[{}] Frame decode error: {}; raw telegram retained", Tag(), ec.message());
                return;
            }
            record.is_scan = IsScanMessage(msg);
            payload = msg.payload.data();
            payload_size = msg.payload.size();
        }
        if (!record.is_scan) {
            stats.non_scan_frames.fetch_add(1, std::memory_order_relaxed);
            HandleNonScanFrame(msg);
            return;
        }

        stats.last_frame_time_us.store(record.frame.receive_timestamp_us, std::memory_order_relaxed);
        ScanData scan;
        const auto ec = first_scan ? ScanDataParser::Parse(payload, payload_size, scan, Tag())
                                  : ScanDataParser::ParseTimestamp(payload, payload_size, scan, Tag());
        if (ec) {
            stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
            g_log.Warn("[{}] Scan time/layout parse error: {}; raw telegram retained", Tag(), ec.message());
            return;
        }
        if (first_scan) {
            first_scan = false;
            VerifyFirstScan(scan);
        }
        if (previous_scan_counter_valid) {
            const auto previous = stats.last_telegram_counter.load(std::memory_order_relaxed);
            const auto expected = static_cast<std::uint32_t>((previous + 1) & 0xFFFF);
            if (scan.telegram_counter != expected) {
                stats.counter_gaps.fetch_add(1, std::memory_order_relaxed);
                g_log.Warn("[{}] Telegram counter discontinuity: expected {}, got {}; diagnostic only, raw order retained",
                           Tag(), expected, scan.telegram_counter);
            }
        }
        previous_scan_counter_valid = true;
        stats.frames_parsed.fetch_add(1, std::memory_order_relaxed);
        stats.last_telegram_counter.store(scan.telegram_counter, std::memory_order_relaxed);
        stats.last_scan_counter.store(scan.scan_counter, std::memory_order_relaxed);
        scan.host_receive_monotonic_us = record.frame.receive_timestamp_us;
        AssessScanTime(scan, time_gate);
        if (config.ntp.enabled && !scan.has_timestamp)
            stats.ntp_status.store(DriverStatistics::NtpStatus::kNoTimestamp, std::memory_order_relaxed);
        record.device_time_unix_us = scan.has_timestamp ? DeviceTimeUnixUs(scan.timestamp) : 0;
        record.clock_step_us = scan.clock_observation.step_us;
        record.clock_quality_flags = scan.clock_observation.flags;
    }


    void Lms4xxxDriver::Impl::HandleFrame(RawFrame &&frame) {
        stats.frames_received.fetch_add(1, std::memory_order_relaxed);
        RawRecord record;
        record.frame = std::move(frame);
        try {
            MonitorFrame(record);
        } catch (const std::exception &e) {
            stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
            fault.store(true, std::memory_order_release);
            ReportError(make_error_code(ErrorCode::kProtocolError), std::string("telegram monitor failed: ") + e.what());
        } catch (...) {
            stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
            fault.store(true, std::memory_order_release);
            ReportError(make_error_code(ErrorCode::kProtocolError), "telegram monitor failed with an unknown exception");
        }
        // A monitor failure never discards its triggering telegram or prevents
        // later stop replies from being received. The owner decides when to stop.
        TelegramCallback cb;
        {
            std::lock_guard lock(callback_mutex);
            cb = telegram_callback;
        }
        if (cb) {
            try {
                cb(std::move(record));
            } catch (const std::exception &e) {
                fault.store(true, std::memory_order_release);
                ReportError(make_error_code(ErrorCode::kProtocolError), std::string("telegram recorder callback failed: ") + e.what());
            } catch (...) {
                fault.store(true, std::memory_order_release);
                ReportError(make_error_code(ErrorCode::kProtocolError), "telegram recorder callback failed with an unknown exception");
            }
        }
    }

} // namespace lms4xxx
