#include "driver.h"

#include <algorithm>
#include <array>
#include <boost/asio/ip/address_v4.hpp>
#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include "cola_b.h"
#include "command_builder.h"
#include "error.h"
#include "frame_receiver.h"
#include "scan_data_parser.h"
#include "ntp_probe.h"
#include "scan_record.h"
#include "scan_verify.h"
#include "ring_buffer.h"
#include "tcp_client.h"
#include "logger.h"
#include "thread_util.h"
#include "time_util.h"
#include "utility.h"


namespace {
    common::DriverLog g_log{"LMS4xxx"};

    constexpr std::size_t kReadBufferSize = 16 * 1024;

    constexpr auto kParseBackoffSleep = std::chrono::microseconds(100);

    // Bound repeated rejected candidates/garbage spans without a valid frame.
    // This resync threshold does not measure lost scans; payload contents and
    // TCP receive chunking affect how many error events are observed.
    constexpr std::uint64_t kMaxConsecutiveFramingErrors = 1000;
    constexpr std::uint32_t kMaxFrameBytes = 64 * 1024; // send_and_receive and FrameReceiver agree

    constexpr int kNtpProbeTimeoutMs = 2000;
    constexpr int kNtpProbeMaxFailures = 3; // consecutive, tolerates UDP loss

    // Telegram time stamp block as ISO-8601 UTC for log lines
    std::string FormatDeviceTime(const lms4xxx::ScanData &scan) {
        if (!scan.has_timestamp) {
            return "<no time stamp block>";
        }
        const auto &ts = scan.timestamp;
        return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z", ts.year, ts.month, ts.day, ts.hour,
                           ts.minute, ts.second, ts.microsecond);
    }

    // "01 00 0D .." for readback-mismatch diagnostics
    std::string HexBytes(const std::vector<std::uint8_t> &bytes) {
        std::string out;
        out.reserve(bytes.size() * 3);
        for (const auto b: bytes) {
            out += fmt::format("{}{:02X}", out.empty() ? "" : " ", b);
        }
        return out;
    }
} // namespace


namespace lms4xxx {
    using FrameRingBuffer = common::RingBuffer<RawFrame>; // receive thread -> parse thread

    struct Lms4xxxDriver::Impl {
        DriverConfig config;

        // --- Components ---
        std::unique_ptr<TcpClient> tcp_client;
        std::unique_ptr<FrameReceiver> frame_receiver;
        std::unique_ptr<FrameRingBuffer> ring_buffer;

        // --- State ---
        std::atomic<ConnectionState> state{ConnectionState::kDisconnected};
        std::atomic<bool> scanning{false};
        std::atomic<bool> receive_running{false};
        std::atomic<bool> parse_running{false};
        // Polled via HasFault(); data collected past it is invalid
        std::atomic<bool> fault{false};
        std::atomic<bool> time_fault{false}; // preserve parsed scans while the owner stops/drains
        std::atomic<bool> stop_stream_requested{false};

        // --- Threads ---
        std::thread receive_thread;
        std::thread parse_thread;
        std::thread ntp_watch_thread;
        std::atomic<bool> ntp_watch_running{false};

        // --- Callbacks ---
        ScanDataCallback scan_callback;
        ErrorCallback error_callback;
        std::mutex callback_mutex; // Protects callback registration (not invocation)

        // --- Statistics ---
        DriverStatistics stats;

        // --- Device self-report ---
        DeviceIdentity identity; // written by ReadIdentity() on the control thread
        DeviceAudit audit; // written at the end of Configure(), read after it

        // Telemetry crosses threads: the owner asks (Write), the parse thread
        // answers (decode), the owner collects.
        mutable std::mutex telemetry_mutex;
        TelemetrySample telemetry_latest;
        bool telemetry_dirty = false;
        // Parse-thread owned; never refreshed by unrelated temperature/state replies.
        std::uint64_t device_warnings_monotonic_us = 0;
        bool device_no_ntp = false;

        // Last sAN seen on the STREAMING path. Once the stream is running the
        // receive thread owns the socket, so a method issued at shutdown cannot
        // use send_and_receive; it is written here and its answer collected by
        // the parse thread (guarded by telemetry_mutex).
        std::string last_method_name;
        int last_method_status = -1;

        // Methods (login, Run, LMCstartmeas) may take far longer than a variable access
        static constexpr int kMethodTimeoutMs = 5000;
        static constexpr int kReadyTimeoutMs = 10000;
        static constexpr int kReadyPollMs = 200;

        [[nodiscard]] int MethodTimeout() const {
            return std::max(config.network.response_timeout_ms, kMethodTimeoutMs);
        }


        explicit Impl(const DriverConfig &cfg) : config(cfg) {
            if (config.name.empty()) {
                config.name = config.device.ip; // always have a usable log tag
            }
        }


        [[nodiscard]] const std::string &Tag() const { return config.name; }


        // The state is bookkeeping for this object only; there is no observer
        // (the owner polls IsScanning/HasFault instead).
        void SetState(ConnectionState new_state) {
            state.store(new_state, std::memory_order_release);
        }


        void ReportError(std::error_code ec, const std::string &detail = "") {
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


        // Drain what is still queued so a half-consumed answer cannot desynchronise the next command
        // (command phase only; the streaming path resyncs inside FrameReceiver)
        void DrainStaleBytes() const {
            constexpr std::size_t kMaxDrain = 64 * 1024;
            std::uint8_t scratch[1024];
            std::size_t dropped = 0;
            while (dropped < kMaxDrain) {
                std::error_code ec;
                const auto n = tcp_client->ReadSome(scratch, sizeof(scratch), ec);
                if (ec) {
                    break;
                }
                if (n == 0) {
                    // SO_RCVTIMEO expired with nothing pending: the socket is
                    // quiet, which is exactly where the next command wants it.
                    break;
                }
                dropped += n;
            }
            if (dropped > 0) {
                g_log.Warn("[{}] Dropped {} stale byte(s) to realign the command stream", Tag(), dropped);
            }
        }


        // Command/response phase only (connect, configure). quiet_sfa: an sFA answer
        // is expected (pre-read, tolerated readback) and logged at trace
        std::error_code SendAndReceive(const std::vector<std::uint8_t> &frame, ColaBMessage &response,
                                         int timeout_ms, bool quiet_sfa = false) const {
            if (!tcp_client || !tcp_client->IsConnected()) {
                return make_error_code(ErrorCode::kNotConnected);
            }

            auto ec = tcp_client->Write(frame);
            if (ec) {
                return ec;
            }

            // Response frame: STX(4) + Length(4) + Data(N) + Checksum(1)
            std::uint8_t header[8];
            std::error_code read_ec;
            auto bytes = tcp_client->Read(header, 8, read_ec, timeout_ms);
            if (read_ec) {
                // A partial read leaves the stream mid-frame; the next command
                // would take this answer's tail for its own header and every
                // command after it would fail. Realign before giving up.
                if (bytes > 0) {
                    DrainStaleBytes();
                }
                return read_ec;
            }
            if (bytes < 8) {
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            if (header[0] != 0x02 || header[1] != 0x02 || header[2] != 0x02 || header[3] != 0x02) {
                g_log.Warn("[{}] Invalid STX in response: {:02X} {:02X} {:02X} {:02X}", Tag(), header[0], header[1],
                           header[2], header[3]);
                DrainStaleBytes();
                return make_error_code(ErrorCode::kProtocolError);
            }

            const auto data_len = ColaBCodec::DecodeUint32(header + 4);

            if (data_len > kMaxFrameBytes) {
                g_log.Warn("[{}] Response data length {} exceeds max", Tag(), data_len);
                DrainStaleBytes(); // the body is still on the wire: realign like the other error paths
                return make_error_code(ErrorCode::kFrameTooLong);
            }

            std::vector<std::uint8_t> data_and_cs(data_len + 1);
            bytes = tcp_client->Read(data_and_cs.data(), data_and_cs.size(), read_ec, timeout_ms);
            if (read_ec) {
                if (bytes > 0 && bytes < data_and_cs.size()) {
                    DrainStaleBytes();
                }
                return read_ec;
            }
            if (bytes < data_and_cs.size()) {
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            const auto computed_cs = ColaBCodec::ComputeChecksum(data_and_cs.data(), data_len);
            const auto received_cs = data_and_cs[data_len];
            if (computed_cs != received_cs) {
                g_log.Warn("[{}] CRC mismatch in response: computed 0x{:02X}, received 0x{:02X}", Tag(), computed_cs,
                           received_cs);
                return make_error_code(ErrorCode::kCrcError);
            }

            const auto decode_ec = ColaBCodec::Decode(data_and_cs.data(), data_len, response, Tag());
            if (decode_ec) {
                return decode_ec;
            }
            if (response.command_type == CommandType::kErrorAnswer) {
                const std::uint16_t code = response.payload.size() >= 2
                                               ? ColaBCodec::DecodeUint16(response.payload.data())
                                               : 0xFFFF;
                g_log.Log(quiet_sfa ? spdlog::level::trace : spdlog::level::warn,
                          "[{}] Device answered sFA {} ({})", Tag(), code, SopasErrorText(code));
                return make_error_code(code == 1 ? ErrorCode::kAccessDenied : ErrorCode::kSopasError);
            }
            return {};
        }


        // sRN <name> -> payload
        std::error_code ReadVariable(std::string_view name, std::vector<std::uint8_t> &payload, int timeout_ms,
                                      bool quiet_sfa = false) {
            ColaBMessage r;
            auto ec = SendAndReceive(CommandBuilder::BuildReadVariable(name), r, timeout_ms, quiet_sfa);
            if (ec) {
                return ec;
            }
            ec = ValidateResponse(r, CommandType::kReadAnswer, name);
            if (ec) {
                return ec;
            }
            payload = r.payload;
            return {};
        }


        // sMN <name> -> sAN <name>; the first payload byte must be ok_byte
        // (nullopt: the method answers without a status byte, e.g. mSCloadappdef)
        std::error_code CallMethod(const std::vector<std::uint8_t> &frame, std::string_view name,
                                    std::optional<std::uint8_t> ok_byte) {
            ColaBMessage r;
            auto ec = SendAndReceive(frame, r, MethodTimeout());
            if (ec) {
                g_log.Error("[{}] {} failed: {}", Tag(), name, ec.message());
                return ec;
            }
            ec = ValidateResponse(r, CommandType::kMethodAnswer, name);
            if (ec) {
                return ec;
            }
            if (ok_byte && (r.payload.empty() || r.payload[0] != *ok_byte)) {
                g_log.Error("[{}] {} rejected by the device (status {})", Tag(), name,
                            r.payload.empty() ? -1 : static_cast<int>(r.payload[0]));
                return make_error_code(ErrorCode::kCommandRejected);
            }
            return {};
        }


        // kTolerateSfa: an sFA on the readback only warns (the effect is verified elsewhere);
        // a readback that differs from what was written is fatal for every row
        enum class Readback { kStrict, kTolerateSfa };

        struct ParamRow {
            std::string_view name;
            std::vector<std::uint8_t> frame;
            Readback readback;
            const char *why;
        };


        // A device that refuses to read a variable back: sFA 1 arrives as kAccessDenied, every other
        // SOPAS code as kSopasError; the pre-read and the readback must agree on this set
        static bool IsTolerableSfa(const std::error_code &ec) {
            return ec == make_error_code(ErrorCode::kSopasError) ||
                   ec == make_error_code(ErrorCode::kAccessDenied);
        }


        // Pre-read (diagnostic), write, read back and byte-compare. `changed` counts
        // rows whose stored value differed from the value written
        std::error_code ApplyParameter(const ParamRow &row, int timeout_ms, int &changed) {
            const auto written = ColaBCodec::ParamsOf(row.frame, row.name.size());

            std::vector<std::uint8_t> before;
            const auto pre_ec = ReadVariable(row.name, before, timeout_ms, true);
            if (pre_ec && !IsTolerableSfa(pre_ec)) {
                g_log.Error("[{}] {} pre-read failed: {}", Tag(), row.name, pre_ec.message());
                return pre_ec;
            }
            const bool had_before = !pre_ec;

            ColaBMessage r;
            auto ec = SendAndReceive(row.frame, r, timeout_ms);
            if (ec) {
                g_log.Error("[{}] {} write failed: {}", Tag(), row.name, ec.message());
                return ec;
            }
            ec = ValidateResponse(r, CommandType::kWriteAnswer, row.name);
            if (ec) {
                g_log.Error("[{}] {} not acknowledged", Tag(), row.name);
                return ec;
            }

            std::vector<std::uint8_t> after;
            ec = ReadVariable(row.name, after, timeout_ms, row.readback == Readback::kTolerateSfa);
            if (ec) {
                if (row.readback == Readback::kTolerateSfa && IsTolerableSfa(ec)) {
                    g_log.Trace("[{}] {} has no readback (sFA): acknowledged only", Tag(), row.name);
                } else {
                    g_log.Error("[{}] {} readback failed: {}", Tag(), row.name, ec.message());
                    return ec;
                }
            } else if (after != written) {
                g_log.Error("[{}] {} readback mismatch: wrote [{}], device reports [{}]", Tag(), row.name,
                            HexBytes(written), HexBytes(after));
                return make_error_code(ErrorCode::kReadbackMismatch);
            }

            const bool differed = had_before && before != written;
            if (differed) {
                ++changed;
            }
            g_log.Trace("[{}] {} set: [{}]{} — {}", Tag(), row.name, HexBytes(written),
                        differed ? fmt::format(" (was [{}])", HexBytes(before)) : "", row.why);
            return {};
        }


        // Length-prefixed (Uint16 or Uint8) or raw string payload
        static std::string PrefixedString(const std::vector<std::uint8_t> &p) {
            if (p.size() >= 2) {
                const std::size_t n16 = ColaBCodec::DecodeUint16(p.data());
                if (n16 > 0 && n16 <= p.size() - 2) {
                    return std::string(reinterpret_cast<const char *>(p.data() + 2), n16);
                }
            }
            if (!p.empty() && p[0] > 0 && p[0] <= p.size() - 1) {
                return std::string(reinterpret_cast<const char *>(p.data() + 1), p[0]);
            }
            return p.empty() ? std::string() : std::string(reinterpret_cast<const char *>(p.data()), p.size());
        }


        // Informational; failures only warn
        void ReadIdentity(int timeout_ms) {
            identity = DeviceIdentity{};
            std::vector<std::uint8_t> p;

            if (!ReadVariable("DeviceIdent", p, timeout_ms)) {
                // <u16 len><designation><u16 len><version>
                std::size_t pos = 0;
                const auto next = [&p, &pos](std::string &out) {
                    if (pos + 2 > p.size()) {
                        return false;
                    }
                    const std::size_t n = ColaBCodec::DecodeUint16(p.data() + pos);
                    pos += 2;
                    if (pos + n > p.size()) {
                        return false;
                    }
                    out.assign(reinterpret_cast<const char *>(p.data() + pos), n);
                    pos += n;
                    return true;
                };
                if (!next(identity.firmware_designation) || !next(identity.firmware_version)) {
                    g_log.Warn("[{}] DeviceIdent answer not understood: [{}]", Tag(), HexBytes(p));
                }
            } else {
                g_log.Warn("[{}] DeviceIdent not readable", Tag());
            }
            if (!ReadVariable("DIornr", p, timeout_ms)) {
                identity.order_number = PrefixedString(p);
            }
            if (!ReadVariable("DItype", p, timeout_ms)) {
                identity.device_type = PrefixedString(p);
            }
            if (!ReadVariable("LocationName", p, timeout_ms)) {
                identity.location_name = PrefixedString(p);
            }
            identity.is_s01_variant = identity.order_number == "1116198";
        }


        // EMActiveCustomerInfo payload -> clear-text messages (manual p.125):
        // Uint16 count, then count x {Uint16 id, Uint16 length, chars}.
        // Pure decode so the command phase and the streaming telemetry path
        // share one implementation.
        static std::vector<std::string> DecodeActiveMessages(const std::vector<std::uint8_t> &p,
                                                            bool *complete = nullptr) {
            if (complete) *complete = false;
            std::vector<std::string> out;
            if (p.size() < 2) {
                return out;
            }
            const std::uint16_t count = ColaBCodec::DecodeUint16(p.data());
            std::size_t pos = 2;
            for (std::uint16_t i = 0; i < count && pos + 4 <= p.size(); ++i) {
                const auto id = ColaBCodec::DecodeUint16(p.data() + pos);
                const std::size_t len = ColaBCodec::DecodeUint16(p.data() + pos + 2);
                pos += 4;
                if (pos + len > p.size()) {
                    break;
                }
                out.push_back(fmt::format("{}: {}", id,
                                          std::string(reinterpret_cast<const char *>(p.data() + pos), len)));
                pos += len;
            }
            if (complete) *complete = pos == p.size() && out.size() == count;
            return out;
        }


        // Active warnings/errors in clear text (EMActiveCustomerInfo)
        void LogActiveMessages(int timeout_ms) {
            std::vector<std::uint8_t> p;
            if (ReadVariable("EMActiveCustomerInfo", p, timeout_ms)) {
                return;
            }
            for (const auto &message: DecodeActiveMessages(p)) {
                g_log.Warn("[{}] Device message {}", Tag(), message);
            }
        }


        // --- device audit (command phase only) --------------------------------
        // Every read is best-effort: the manual documents these variables but not
        // for every firmware, and a silent field costs metadata, never the run.
        void ReadAudit(int timeout_ms) {
            audit = DeviceAudit{};
            std::vector<std::uint8_t> p;

            if (!ReadVariable("OPcurtmpdev", p, timeout_ms, true) && p.size() >= 4) {
                audit.temperature_c = ColaBCodec::DecodeFloat(p.data());
            }
            if (!ReadVariable("ODoprh", p, timeout_ms, true) && p.size() >= 4) {
                audit.operating_hours_deci = ColaBCodec::DecodeUint32(p.data());
            }
            if (!ReadVariable("ODpwrc", p, timeout_ms, true) && p.size() >= 4) {
                audit.power_on_count = ColaBCodec::DecodeUint32(p.data());
            }
            if (!ReadVariable("SCdevicestate", p, timeout_ms, true) && !p.empty()) {
                audit.device_state = p[0];
            }
            // sRA LMPscancfg: freq(u32) reserved(i16) resolution(u32) start(i32) stop(i32)
            if (!ReadVariable("LMPscancfg", p, timeout_ms, true) && p.size() >= 18) {
                audit.scan_frequency_centi_hz = ColaBCodec::DecodeUint32(p.data());
                audit.angular_resolution_1e4 = ColaBCodec::DecodeUint32(p.data() + 6);
                audit.start_angle_1e4 = ColaBCodec::DecodeInt32(p.data() + 10);
                audit.stop_angle_1e4 = ColaBCodec::DecodeInt32(p.data() + 14);
            }
            // sRA IOlasc: source(u8) delay_start(u16) delay_stop(u16) timeout(u16) reserved(u8)
            if (!ReadVariable("IOlasc", p, timeout_ms, true) && p.size() >= 8) {
                audit.laser_trigger_source = p[0];
                audit.laser_timeout_s = ColaBCodec::DecodeUint16(p.data() + 5);
            }
            if (!ReadVariable("SYtype", p, timeout_ms, true) && !p.empty()) {
                audit.motor_sync_role = p[0];
            }
            if (!ReadVariable("SYphas", p, timeout_ms, true) && p.size() >= 4) {
                audit.motor_sync_phase_deg = ColaBCodec::DecodeUint32(p.data()) / 10000.0;
            }

            // Continuous output requires free-running laser control (p.17), and a
            // laser timeout would switch the laser off mid-survey (p.77).
            if (audit.laser_trigger_source && *audit.laser_trigger_source != 0) {
                g_log.Warn("[{}] Laser control is trigger source {} (0 = free-running); continuous measurement "
                           "output expects free-running (manual p.17/p.77)",
                           Tag(), *audit.laser_trigger_source);
            }
            if (audit.laser_timeout_s && *audit.laser_timeout_s != 0) {
                g_log.Warn("[{}] Laser timeout is {} s: the device will switch the laser off that long into the "
                           "Run (manual p.77)", Tag(), *audit.laser_timeout_s);
            }
            // Motor synchronisation is deliberately never written by this driver,
            // but it is an Interfaces parameter and mSCloadappdef does not reset
            // those (p.79), so a role left behind by SOPAS outlives everything.
            if (audit.motor_sync_role && *audit.motor_sync_role != 0) {
                g_log.Warn("[{}] Device has motor synchronisation configured (SYtype={}, phase={:.2f} deg). This "
                           "driver never sets it and mSCloadappdef does not reset Interfaces parameters (manual "
                           "p.79) — clear it in SOPAS if it is not intended",
                           Tag(), *audit.motor_sync_role, audit.motor_sync_phase_deg.value_or(0.0));
            }
            g_log.Info("[{}] Device audit: temperature={} C, operating_hours={} h, power_on_count={}", Tag(),
                       audit.temperature_c ? fmt::format("{:.1f}", *audit.temperature_c) : "n/a",
                       audit.operating_hours_deci ? fmt::format("{:.1f}", *audit.operating_hours_deci / 10.0) : "n/a",
                       audit.power_on_count ? fmt::format("{}", *audit.power_on_count) : "n/a");
        }


        // --- telemetry while streaming ----------------------------------------
        // Only WRITES: the receive thread owns the socket, so the answers come
        // back through FrameReceiver and land in HandleNonScanFrame().
        void RequestTelemetry() {
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


        void HandleNonScanFrame(const ColaBMessage &msg) {
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


        // Issue a method while the stream owns the socket and wait for its sAN.
        // Returns false on a timeout or an unexpected status byte; NOTE the
        // status polarity differs per command (manual): LMCstandby/startmeas/
        // stopmeas answer 0 = success, SetAccessMode/Run answer 1 = success.
        bool CallMethodStreaming(const std::vector<std::uint8_t> &frame, const std::string &name,
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


        bool TakeTelemetry(TelemetrySample &out) {
            std::lock_guard lock(telemetry_mutex);
            if (!telemetry_dirty) {
                return false;
            }
            out = telemetry_latest;
            telemetry_dirty = false;
            return true;
        }


        // Poll SCdevicestate until Ready (manual p.46: the device needs time after Run)
        std::error_code WaitDeviceReady(int timeout_ms) {
            const auto started = std::chrono::steady_clock::now();
            const auto deadline = started + std::chrono::milliseconds(kReadyTimeoutMs);
            while (true) {
                std::vector<std::uint8_t> p;
                auto ec = ReadVariable("SCdevicestate", p, std::max(timeout_ms, MethodTimeout()));
                if (ec) {
                    g_log.Error("[{}] SCdevicestate read failed: {}", Tag(), ec.message());
                    return ec;
                }
                const int state = p.empty() ? -1 : static_cast<int>(p[0]);
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
                if (state == 1) {
                    g_log.Trace("[{}] Device ready after {} ms", Tag(), elapsed);
                    return {};
                }
                if (state == 2) {
                    g_log.Error("[{}] Device reports the error state", Tag());
                    LogActiveMessages(timeout_ms);
                    return make_error_code(ErrorCode::kDeviceNotReady);
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    g_log.Error("[{}] Device not ready after {} ms (SCdevicestate {})", Tag(), elapsed, state);
                    LogActiveMessages(timeout_ms);
                    return make_error_code(ErrorCode::kDeviceNotReady);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kReadyPollMs));
            }
        }


        std::error_code ValidateResponse(const ColaBMessage &msg, std::string_view expected_type,
                                          std::string_view expected_name) const {
            if (msg.command_type != expected_type) {
                g_log.Warn("[{}] Unexpected response type '{}', expected '{}'", Tag(), msg.command_type, expected_type);
                return make_error_code(ErrorCode::kUnexpectedResponse);
            }
            if (msg.command_name != expected_name) {
                g_log.Warn("[{}] Unexpected response name '{}', expected '{}'", Tag(), msg.command_name, expected_name);
                return make_error_code(ErrorCode::kUnexpectedResponse);
            }
            return {};
        }


        // Real-time scheduling; fails without CAP_SYS_NICE (Docker): warn and degrade
        void ConfigureReceiveThread() {
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


        void ReceiveLoop() {
            g_log.Trace("[{}] Receive thread started", Tag());

            std::vector<std::uint8_t> buf(kReadBufferSize);

            while (receive_running.load(std::memory_order_acquire)) {
                std::error_code ec;
                const auto n = tcp_client->ReadSome(buf.data(), buf.size(), ec);

                if (ec) {
                    if (receive_running.load(std::memory_order_relaxed)) {
                        g_log.Error("[{}] Receive thread: read error: {}", Tag(), ec.message());
                        fault.store(true, std::memory_order_release);
                        ReportError(ec, "receive thread read error");
                        SetState(ConnectionState::kError);
                    }
                    break;
                }

                if (n > 0) {
                    stats.bytes_received.fetch_add(n, std::memory_order_relaxed);
                    frame_receiver->Feed(buf.data(), n);
                }
            }

            g_log.Trace("[{}] Receive thread stopped", Tag());
        }


        // The first streamed telegram is the proof that the configuration took effect
        void VerifyFirstScan(const ScanData &scan) {
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


        bool ProbeNtpServer(std::string &detail) const {
            return Ntp::ProbeServer(config.ntp.server, kNtpProbeTimeoutMs, detail);
        }


        // Host-to-server reachability watch, independent of device clock quality.
        // kNtpProbeMaxFailures consecutive misses request an orderly stop.
        void NtpWatchLoop() {
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


        // Plausibility is not an NTP lock. Preserve raw scans and their quality
        // flags, including the sample which requests an orderly time-fault stop.
        struct TimeGate {
            std::chrono::steady_clock::time_point deadline;
            bool plausible_seen = false;
            bool anomaly_seen = false;
            ClockQualityTracker tracker;
        };

        void AssessScanTime(ScanData &scan, TimeGate &gate) {
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

            if (observation.flags & ClockFlag::kBackwardUtc) {
                const auto count = stats.utc_backwards.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count == 1) g_log.Warn("[{}] Device UTC moved backwards; raw scan order/time retained, timing quality degraded", Tag());
                gate.anomaly_seen = true;
            }
            if (observation.flags & ClockFlag::kRepeatedUtc)
                stats.utc_repeated.fetch_add(1, std::memory_order_relaxed);
            const bool large_step = (observation.flags & (ClockFlag::kStepExceeded | ClockFlag::kUptimeDiscontinuity)) != 0;
            if (large_step) {
                stats.clock_step_events.fetch_add(1, std::memory_order_relaxed);
                gate.anomaly_seen = true;
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


        void ParseLoop() {
            g_log.Trace("[{}] Parse thread started", Tag());

            bool first_frame = true;
            TimeGate time_gate;
            time_gate.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config.ntp.lock_timeout_s);
            if (config.ntp.enabled) {
                stats.ntp_status.store(DriverStatistics::NtpStatus::kNotLocked, std::memory_order_relaxed);
            }

            while (true) {
                // A time fault does not invalidate native range/intensity data.
                // Keep draining its scans and command replies during orderly stop.
                if (fault.load(std::memory_order_acquire) && !time_fault.load(std::memory_order_acquire)) {
                    g_log.Trace("[{}] Parse thread stopping: fault latched", Tag());
                    break;
                }

                RawFrame frame;
                if (!ring_buffer->try_pop(frame)) {
                    if (!parse_running.load(std::memory_order_acquire)) {
                        break; // producer has joined; the queue is now drained
                    }
                    std::this_thread::sleep_for(kParseBackoffSleep);
                    continue;
                }

                ColaBMessage msg;
                auto ec = ColaBCodec::Decode(frame.data.data(), frame.data.size(), msg, Tag());
                if (ec) {
                    stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
                    g_log.Warn("[{}] Frame decode error: {}", Tag(), ec.message());
                    continue;
                }

                if (!IsScanMessage(msg)) {
                    // Telemetry answers (sRA) and anything else the device sends
                    // mid-stream. Never silently discarded: an unexpected reply
                    // here is the only visible symptom of a desynchronised link.
                    stats.non_scan_frames.fetch_add(1, std::memory_order_relaxed);
                    HandleNonScanFrame(msg);
                    continue;
                }

                ScanData scan;
                ec = ScanDataParser::Parse(msg.payload.data(), msg.payload.size(), scan, Tag());
                if (ec) {
                    stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
                    g_log.Warn("[{}] Scan data parse error: {}", Tag(), ec.message());
                    continue;
                }

                if (!first_frame) {
                    const auto problems = lms4xxx::VerifyScanContent(scan, config.scan, config.ntp.enabled);
                    if (!problems.empty()) {
                        stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
                        fault.store(true, std::memory_order_release);
                        ReportError(make_error_code(ErrorCode::kInvalidConfig), "scan content changed: " + problems);
                        break;
                    }
                }
                if (first_frame) {
                    VerifyFirstScan(scan);
                    if (config.ntp.enabled && !scan.has_timestamp) {
                        // Already faulted; record the reason for ntp=
                        stats.ntp_status.store(DriverStatistics::NtpStatus::kNoTimestamp, std::memory_order_relaxed);
                    }
                    // The mismatching telegram is itself invalid: do not hand it
                    // to the recorder on the way out.
                    if (fault.load(std::memory_order_acquire)) {
                        break;
                    }
                }

                if (!first_frame) {
                    const auto prev_counter = stats.last_telegram_counter.load(std::memory_order_relaxed);
                    const auto expected = static_cast<std::uint32_t>((prev_counter + 1) & 0xFFFF);
                    if (scan.telegram_counter != expected) {
                        const auto gap = (scan.telegram_counter >= expected)
                                             ? scan.telegram_counter - expected
                                             : (0x10000 + scan.telegram_counter - expected);
                        stats.counter_gaps.fetch_add(1, std::memory_order_relaxed);
                        g_log.Warn("[{}] Telegram counter gap: expected {}, got {} (missed ~{} frames)", Tag(),
                                   expected, scan.telegram_counter, gap);
                    }
                }
                first_frame = false;
                stats.frames_parsed.fetch_add(1, std::memory_order_relaxed);

                stats.last_telegram_counter.store(scan.telegram_counter, std::memory_order_relaxed);
                stats.last_scan_counter.store(scan.scan_counter, std::memory_order_relaxed);
                stats.last_frame_time_us.store(frame.receive_timestamp_us, std::memory_order_relaxed);

                scan.host_receive_monotonic_us = frame.receive_timestamp_us;
                AssessScanTime(scan, time_gate);

                ScanDataCallback cb;
                {
                    std::lock_guard lock(callback_mutex);
                    cb = scan_callback;
                }
                if (cb) {
                    try {
                        cb(scan);
                    } catch (const std::exception &e) {
                        g_log.Warn("[{}] Scan callback threw: {}", Tag(), e.what());
                    }
                }
            }

            g_log.Trace("[{}] Parse thread stopped", Tag());
        }
    };


    Lms4xxxDriver::Lms4xxxDriver(const DriverConfig &config) : impl_(std::make_unique<Impl>(config)) {
    }


    Lms4xxxDriver::~Lms4xxxDriver() {
        if (impl_->scanning.load(std::memory_order_relaxed)) {
            StopScanning();
        }
        if (impl_->tcp_client && impl_->tcp_client->IsConnected()) {
            Disconnect();
        }
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
        g_log.Info("[{}] Connected", impl_->Tag());
        return {};
    }


    std::error_code Lms4xxxDriver::Configure() {
        if (!impl_->tcp_client || !impl_->tcp_client->IsConnected()) {
            return make_error_code(ErrorCode::kNotConnected);
        }

        impl_->SetState(ConnectionState::kConfiguring);
        const int timeout = impl_->config.network.config_timeout_ms;
        const auto &cfg = impl_->config;
        const auto fail = [this](std::error_code ec) {
            impl_->SetState(ConnectionState::kError);
            return ec;
        };

        impl_->ReadIdentity(timeout);
        const auto &id = impl_->identity;
        g_log.Info("[{}] Device: {} fw {}, order {}{}, type {}, name '{}'", impl_->Tag(), id.firmware_designation,
                   id.firmware_version, id.order_number,
                   id.is_s01_variant ? " (LMS4124R-13000S01: quality-flagged points stay valid)" : "", id.device_type,
                   id.location_name);

        // 1. Login (the laser is off until Run)
        if (auto ec = impl_->CallMethod(CommandBuilder::BuildLogin(), "SetAccessMode", 0x01)) {
            return fail(ec);
        }
        g_log.Trace("[{}] Logged in as Authorized Client", impl_->Tag());

        // 2. Baseline: application defaults (p.79) — every "Application" parameter back to
        // factory, interface/IP settings untouched. Answered without a status byte. Whether
        // it logs the client out is undocumented, so log in again.
        if (auto ec = impl_->CallMethod(CommandBuilder::BuildLoadAppDefaults(), "mSCloadappdef", std::nullopt)) {
            return fail(ec);
        }
        g_log.Trace("[{}] Application defaults loaded", impl_->Tag());
        if (auto ec = impl_->CallMethod(CommandBuilder::BuildLogin(), "SetAccessMode", 0x01)) {
            return fail(ec);
        }

        // 3. What the recording depends on, written and read back regardless of the defaults
        using R = Impl::Readback;
        std::vector<Impl::ParamRow> rows = {
            {
                "LMDscandatacfg",
                CommandBuilder::BuildScanDataConfig(cfg.scan.remission, cfg.ntp.enabled),
                R::kTolerateSfa,
                cfg.ntp.enabled
                    ? "DIST + remission + ANGL + QLTY, time stamp, every scan"
                    : "DIST + remission + ANGL + QLTY, no time stamp (NTP off), every scan"
            },
            {
                "LMPoutputRange",
                CommandBuilder::BuildOutputRange(),
                R::kStrict,
                "55..125 deg @ 1/12 deg (841 points)"
            },
            // Filters
            {
                "LFPmeanfilter",
                CommandBuilder::BuildMeanFilter(false, 2),
                R::kStrict,
                "mean filter off"
            },
            {
                "LFPmedianfilter",
                CommandBuilder::BuildMedianFilter(false),
                R::kStrict,
                "median filter off"
            },
            {
                "LFPfrontendEdgefilter",
                CommandBuilder::BuildFrontendEdgeFilter(ScanFixed::kFrontendEdgeFilterOff),
                R::kStrict,
                "front-end edge filter off"
            },
            {
                "LFPedgefilter",
                CommandBuilder::BuildEdgeFilter(false),
                R::kStrict,
                "edge filter off"
            },
            {
                "LFPcubicareafilter",
                CommandBuilder::BuildCubicAreaFilter(ScanFixed::kCubicAreaFilterOff, 0, ScanFixed::kCubicMaxDist1e4mm,
                                                     -ScanFixed::kCubicExpansion1e4mm, ScanFixed::kCubicExpansion1e4mm),
                R::kStrict,
                "rectangular filter off"
            },
            {
                "LFPglossfilter",
                CommandBuilder::BuildGlossFilter(false),
                R::kStrict,
                "gloss compensation off"
            },
            // device time
            {
                "TSCTCtimezone",
                CommandBuilder::BuildSetNtpTimezone(ScanFixed::kTimezoneUtc),
                R::kTolerateSfa,
                "device timestamps in UTC"
            },
        };
        if (cfg.ntp.enabled) {
            boost::system::error_code bec;
            const auto addr = boost::asio::ip::make_address_v4(cfg.ntp.server, bec);
            if (bec) {
                g_log.Error("[{}] Invalid NTP server IP '{}'", impl_->Tag(), cfg.ntp.server);
                return fail(make_error_code(ErrorCode::kInvalidConfig));
            }
            rows.push_back({
                "TSCRole",
                CommandBuilder::BuildSetTimeSyncRole(static_cast<std::uint8_t>(TscRole::kClient)),
                R::kTolerateSfa,
                "NTP client"
            });
            rows.push_back({
                "TSCTCSrvAddr",
                CommandBuilder::BuildSetNtpServer(addr.to_bytes()),
                R::kTolerateSfa,
                "NTP server"
            });
            rows.push_back({
                "TSCTCupdatetime",
                CommandBuilder::BuildSetNtpUpdateTime(cfg.ntp.sync_interval_s),
                R::kTolerateSfa,
                "NTP update interval"
            });
        } else {
            rows.push_back({
                "TSCRole",
                CommandBuilder::BuildSetTimeSyncRole(static_cast<std::uint8_t>(TscRole::kOff)),
                R::kTolerateSfa,
                "NTP off"
            });
        }
        int changed = 0;
        for (const auto &row: rows) {
            if (auto ec = impl_->ApplyParameter(row, timeout, changed)) {
                return fail(ec);
            }
        }

        // 4. NTP effect: server liveness, not a device-clock comparison (see DriverStatistics)
        if (cfg.ntp.enabled) {
            std::string ntp_detail;
            if (!impl_->ProbeNtpServer(ntp_detail)) {
                g_log.Error("[{}] NTP server {} check failed: {}", impl_->Tag(), cfg.ntp.server, ntp_detail);
                return fail(make_error_code(ErrorCode::kInvalidConfig));
            }
            impl_->stats.ntp_server_reachable.store(true, std::memory_order_relaxed);
            impl_->stats.ntp_status.store(DriverStatistics::NtpStatus::kUnverified, std::memory_order_relaxed);
            impl_->stats.ntp_configured_at_us.store(common::TimeUtil::RealtimeNowUs(), std::memory_order_relaxed);
            g_log.Info("[{}] NTP configured: server={} ({}), interval={} s, timezone=UTC", impl_->Tag(),
                       cfg.ntp.server, ntp_detail, cfg.ntp.sync_interval_s);
        }

        // 5. Start measurement, activate everything (Run logs out and turns the laser on), wait for Ready
        if (auto ec = impl_->CallMethod(CommandBuilder::BuildStartMeasurement(), "LMCstartmeas", 0x00)) {
            return fail(ec);
        }
        if (auto ec = impl_->CallMethod(CommandBuilder::BuildRun(), "Run", 0x01)) {
            return fail(ec);
        }
        if (auto ec = impl_->WaitDeviceReady(timeout)) {
            return fail(ec);
        }

        // 6. What the device says about itself, once the configuration is live.
        // Queries need no user level (manual p.69), so this runs fine after Run
        // logged the session out — and it is the last command-phase traffic, so
        // a device that does not answer costs metadata and nothing else.
        impl_->ReadAudit(timeout);

        g_log.Info("[{}] Device configured", impl_->Tag());

        impl_->SetState(ConnectionState::kConnected);
        return {};
    }


    std::error_code Lms4xxxDriver::StartScanning() {
        if (!impl_->tcp_client || !impl_->tcp_client->IsConnected()) {
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
            kMaxFrameBytes, impl_->Tag());

        {
            auto frame = CommandBuilder::BuildStartStream();
            ColaBMessage response;
            auto ec = impl_->SendAndReceive(frame, response, impl_->config.network.response_timeout_ms);
            if (ec) {
                g_log.Error("[{}] Start stream command failed: {}", impl_->Tag(), ec.message());
                return ec;
            }
            auto validate_ec = impl_->ValidateResponse(response, CommandType::kEventAnswer, "LMDscandata");
            if (validate_ec) {
                return validate_ec;
            }

            if (response.payload.empty() || response.payload[0] != 0x01) {
                g_log.Error("[{}] Start stream rejected", impl_->Tag());
                return make_error_code(ErrorCode::kCommandRejected);
            }
        }

        impl_->scanning.store(true, std::memory_order_release);
        impl_->receive_running.store(true, std::memory_order_release);
        impl_->parse_running.store(true, std::memory_order_release);

        impl_->parse_thread = std::thread([this]() { impl_->ParseLoop(); });
        impl_->receive_thread = std::thread([this]() { impl_->ReceiveLoop(); });
        if (impl_->config.ntp.enabled) {
            impl_->ntp_watch_running.store(true, std::memory_order_release);
            impl_->ntp_watch_thread = std::thread([this]() { impl_->NtpWatchLoop(); });
        }

        impl_->ConfigureReceiveThread();

        impl_->SetState(ConnectionState::kScanning);
        g_log.Trace("[{}] Scanning started (ring buffer capacity: {} frames)", impl_->Tag(),
                    impl_->ring_buffer->capacity());
        return {};
    }


    std::error_code Lms4xxxDriver::StopScanning() {
        if (!impl_->scanning.load(std::memory_order_relaxed)) {
            return make_error_code(ErrorCode::kNotScanning);
        }

        g_log.Trace("[{}] Stopping scan...", impl_->Tag());

        // Stop the stream and park the laser while the worker threads still decode answers. `sMN Run`
        // logged this session out, so LMCstandby needs Authorized Client again (manual p.74); a refused
        // standby leaves the laser on (p.17). Skipped when the parse thread has already given up
        if (impl_->tcp_client && impl_->tcp_client->IsConnected()) {
            constexpr int kShutdownMethodTimeoutMs = 1000;
            impl_->stop_stream_requested.store(true, std::memory_order_release);
            if (const auto ec = impl_->tcp_client->Write(CommandBuilder::BuildStopStream())) {
                g_log.Warn("[{}] Failed to send stop stream command (non-fatal): {}", impl_->Tag(), ec.message());
            }
            if (impl_->parse_running.load(std::memory_order_acquire) &&
                (!impl_->fault.load(std::memory_order_acquire) || impl_->time_fault.load(std::memory_order_acquire))) {
                // SetAccessMode answers 1 = success, LMCstandby answers 0 = success.
                const bool logged_in = impl_->CallMethodStreaming(CommandBuilder::BuildLogin(), "SetAccessMode",
                                                                   0x01, kShutdownMethodTimeoutMs);
                if (logged_in &&
                    impl_->CallMethodStreaming(CommandBuilder::BuildStandby(), "LMCstandby", 0x00,
                                                 kShutdownMethodTimeoutMs)) {
                    g_log.Info("[{}] Laser parked (standby); the motor keeps turning", impl_->Tag());
                } else {
                    g_log.Warn("[{}] Could not put the device into standby — THE LASER IS STILL ON until the "
                               "device is powered down (manual p.17)", impl_->Tag());
                }
            } else {
                g_log.Warn("[{}] Skipped the standby handshake (the run had already failed); the laser stays on "
                           "until the device is powered down", impl_->Tag());
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
        while (impl_->ring_buffer->try_pop(remaining)) {
            ++discarded;
        }
        if (discarded > 0) {
            impl_->stats.frames_dropped.fetch_add(discarded, std::memory_order_relaxed);
            g_log.Warn("[{}] {} raw frames discarded after a parse fault", impl_->Tag(), discarded);
        }

        impl_->SetState(ConnectionState::kConnected);
        g_log.Trace("[{}] Scanning stopped", impl_->Tag());
        return {};
    }


    void Lms4xxxDriver::Disconnect() {
        if (impl_->scanning.load(std::memory_order_relaxed)) {
            StopScanning();
        }

        if (impl_->tcp_client) {
            impl_->tcp_client->Disconnect();
        }

        impl_->frame_receiver.reset();
        impl_->ring_buffer.reset();

        impl_->SetState(ConnectionState::kDisconnected);
        g_log.Trace("[{}] Disconnected", impl_->Tag());
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
