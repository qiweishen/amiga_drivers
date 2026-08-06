#include "lms4xxx_driver.h"

#include <arpa/inet.h>
#include <array>
#include <boost/asio/ip/address_v4.hpp>
#include <chrono>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <initializer_list>
#include <mutex>
#include <string_view>
#include <thread>

#include "lms4xxx_cola_b.h"
#include "lms4xxx_command_builder.h"
#include "lms4xxx_error.h"
#include "lms4xxx_frame_receiver.h"
#include "lms4xxx_scan_data_parser.h"
#include "lms4xxx_spsc_ring_buffer.h"
#include "lms4xxx_tcp_client.h"
#include "logger.h"
#include "thread_util.h"
#include "utility.h"


namespace {
    Common::DriverLog g_log{"LMS4xxx"};

    // Read buffer size for the receive thread (16 KB).
    constexpr std::size_t kReadBufferSize = 16 * 1024;

    // Parse thread backoff sleep when ring buffer is empty.
    constexpr auto kParseBackoffSleep = std::chrono::microseconds(100);

    // NTP server probe (RFC 4330 SNTP client query, check-only — nothing is
    // ever corrected from it): one query at Configure() plus a periodic watch
    // while scanning. Probing the SERVER, deliberately not the device clock:
    // a previously-synced device keeps near-correct time and would mask a
    // dead server from any offset-based check.
    constexpr std::uint16_t kNtpPort = 123;
    constexpr int kNtpProbeTimeoutMs = 2000;
    constexpr int kNtpProbeMaxFailures = 3; // consecutive, tolerates UDP loss

    // "01 00 0D .." for readback-mismatch diagnostics.
    std::string hex_bytes(const std::vector<std::uint8_t> &bytes) {
        std::string out;
        out.reserve(bytes.size() * 3);
        for (const auto b: bytes) {
            out += fmt::format("{}{:02X}", out.empty() ? "" : " ", b);
        }
        return out;
    }
} // namespace


namespace LMS4xxx {
    struct LMS4xxxDriver::Impl {
        DriverConfig config;

        // --- Components ---
        std::unique_ptr<TCPClient> tcp_client;
        std::unique_ptr<FrameReceiver> frame_receiver;
        std::unique_ptr<FrameRingBuffer> ring_buffer;

        // --- State ---
        std::atomic<ConnectionState> state{ConnectionState::kDisconnected};
        std::atomic<bool> scanning{false};
        std::atomic<bool> receive_running{false};
        std::atomic<bool> parse_running{false};
        // Fatal runtime fault (verification failure / receive-channel error);
        // polled by the owner via HasFault() — data collected past it is invalid
        std::atomic<bool> fault{false};

        // --- Threads ---
        std::thread receive_thread;
        std::thread parse_thread;
        std::thread ntp_watch_thread;
        std::atomic<bool> ntp_watch_running{false};

        // --- Callbacks ---
        ScanDataCallback scan_callback;
        ConnectionStateCallback connection_callback;
        ErrorCallback error_callback;
        std::mutex callback_mutex; // Protects callback registration (not invocation)

        // --- Statistics ---
        DriverStatistics stats;

        // --- Response handling for command/response phase ---
        std::mutex response_mutex;
        std::vector<std::uint8_t> response_buffer;


        explicit Impl(const DriverConfig &cfg) : config(cfg) {
            if (config.name.empty()) {
                config.name = config.device.ip; // always have a usable log tag
            }
        }


        // Instance tag for log lines, e.g. "[front_left_laser] ..."
        [[nodiscard]] const std::string &tag() const { return config.name; }


        // Set connection state and invoke callback.
        void set_state(ConnectionState new_state) {
            state.store(new_state, std::memory_order_release);
            ConnectionStateCallback cb;
            {
                std::lock_guard lock(callback_mutex);
                cb = connection_callback;
            }
            if (cb) {
                try {
                    cb(new_state);
                } catch (const std::exception &e) {
                    g_log.warn("[{}] Connection callback threw: {}", tag(), e.what());
                }
            }
        }


        // Invoke error callback.
        void report_error(std::error_code ec, const std::string &detail = "") {
            ErrorCallback cb;
            {
                std::lock_guard lock(callback_mutex);
                cb = error_callback;
            }
            if (cb) {
                try {
                    cb(ec, detail);
                } catch (const std::exception &e) {
                    g_log.warn("[{}] Error callback threw: {}", tag(), e.what());
                }
            }
        }


        // Send a CoLa B frame and read a synchronous response
        // Used during the command/response phase (connect, configure)
        std::error_code send_and_receive(const std::vector<std::uint8_t> &frame, CoLaBMessage &response,
                                         int timeout_ms) {
            if (!tcp_client || !tcp_client->IsConnected()) {
                return make_error_code(ErrorCode::kNotConnected);
            }

            auto ec = tcp_client->Write(frame);
            if (ec) {
                return ec;
            }

            // Read response frame synchronously.
            // Frame format: STX(4) + Length(4) + Data(N) + Checksum(1)

            // 1. Read STX (4 bytes)
            std::uint8_t header[8];
            std::error_code read_ec;
            auto bytes = tcp_client->Read(header, 8, read_ec, timeout_ms);
            if (read_ec) {
                return read_ec;
            }
            if (bytes < 8) {
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            // Verify STX
            if (header[0] != 0x02 || header[1] != 0x02 || header[2] != 0x02 || header[3] != 0x02) {
                g_log.warn("[{}] Invalid STX in response: {:02X} {:02X} {:02X} {:02X}", tag(), header[0], header[1],
                           header[2], header[3]);
                return make_error_code(ErrorCode::kProtocolError);
            }

            // 2. Read data length (big-endian uint32)
            const auto data_len = CoLaBCodec::DecodeUint32(header + 4);

            if (data_len > 64 * 1024) {
                g_log.warn("[{}] Response data length {} exceeds max", tag(), data_len);
                return make_error_code(ErrorCode::kFrameTooLong);
            }

            // 3. Read Data + Checksum
            std::vector<std::uint8_t> data_and_cs(data_len + 1);
            bytes = tcp_client->Read(data_and_cs.data(), data_and_cs.size(), read_ec, timeout_ms);
            if (read_ec) {
                return read_ec;
            }
            if (bytes < data_and_cs.size()) {
                return make_error_code(ErrorCode::kFrameTooShort);
            }

            // 4. Validate CRC8
            const auto computed_cs = CoLaBCodec::ComputeChecksum(data_and_cs.data(), data_len);
            const auto received_cs = data_and_cs[data_len];
            if (computed_cs != received_cs) {
                g_log.warn("[{}] CRC mismatch in response: computed 0x{:02X}, received 0x{:02X}", tag(), computed_cs,
                           received_cs);
                return make_error_code(ErrorCode::kCrcError);
            }

            // 5. Decode message
            return CoLaBCodec::Decode(data_and_cs.data(), data_len, response, tag());
        }


        // Parameter bytes of a frame built by CoLaBCodec::Encode: everything
        // after "sWN <name> " and before the checksum (empty when no params).
        // Matches CoLaBCodec::Decode's payload extraction, so a readback payload
        // can be byte-compared against what was written.
        static std::vector<std::uint8_t> params_of(const std::vector<std::uint8_t> &frame,
                                                   std::size_t name_len) {
            const std::size_t start = 8 + 3 + 1 + name_len + 1; // header + type + SP + name + SP
            const std::size_t end = frame.size() - 1; // checksum
            if (start >= end) {
                return {};
            }
            return {
                frame.begin() + static_cast<std::ptrdiff_t>(start),
                frame.begin() + static_cast<std::ptrdiff_t>(end)
            };
        }


        // fx10-style verify-set: write the variable (sWN -> sWA), then read it
        // back (sRN -> sRA) and byte-compare against what was written — a device
        // that acknowledges but stores something else is exactly the silent
        // failure this guards against. Only for variables with documented
        // readback support (e.g. LMPoutputRange).
        std::error_code write_and_verify(std::string_view var_name,
                                         const std::vector<std::uint8_t> &write_frame, int timeout_ms) {
            CoLaBMessage response;
            auto ec = send_and_receive(write_frame, response, timeout_ms);
            if (ec) {
                g_log.error("[{}] {} write failed: {}", tag(), var_name, ec.message());
                return ec;
            }
            ec = validate_response(response, CommandType::kWriteAnswer, var_name);
            if (ec) {
                return ec;
            }

            ec = send_and_receive(CommandBuilder::BuildReadVariable(var_name), response, timeout_ms);
            if (ec) {
                g_log.error("[{}] {} readback failed: {}", tag(), var_name, ec.message());
                return ec;
            }
            ec = validate_response(response, CommandType::kReadAnswer, var_name);
            if (ec) {
                return ec;
            }

            const auto written = params_of(write_frame, var_name.size());
            if (response.payload != written) {
                g_log.error("[{}] {} readback mismatch: wrote [{}], device reports [{}]", tag(), var_name,
                            hex_bytes(written), hex_bytes(response.payload));
                return make_error_code(ErrorCode::kUnexpectedResponse);
            }
            g_log.trace("[{}] {} has been set (readback verified)", tag(), var_name);
            return {};
        }


        // Validate a command response matches the expected command type and name.
        std::error_code validate_response(const CoLaBMessage &msg, std::string_view expected_type,
                                          std::string_view expected_name) {
            if (msg.command_type != expected_type) {
                g_log.warn("[{}] Unexpected response type '{}', expected '{}'", tag(), msg.command_type, expected_type);
                return make_error_code(ErrorCode::kUnexpectedResponse);
            }
            if (msg.command_name != expected_name) {
                g_log.warn("[{}] Unexpected response name '{}', expected '{}'", tag(), msg.command_name, expected_name);
                return make_error_code(ErrorCode::kUnexpectedResponse);
            }
            return {};
        }


        // Configure the receive thread for real-time scheduling (failures are
        // expected without CAP_SYS_NICE, e.g. inside Docker: warn and degrade).
        void configure_receive_thread() {
            if (config.network.receive_thread_priority > 0) {
                const int prio = config.network.receive_thread_priority;
                if (const int ret = Common::ThreadUtil::SetRealtimePriority(receive_thread, prio); ret != 0) {
                    g_log.warn("[{}] Failed to set SCHED_FIFO priority {}: {} (requires root or CAP_SYS_NICE)", tag(),
                               prio, std::strerror(ret));
                } else {
                    g_log.trace("[{}] Receive thread: SCHED_FIFO priority {}", tag(), prio);
                }
            }

            if (config.network.receive_thread_cpu >= 0) {
                const int cpu = config.network.receive_thread_cpu;
                if (const int ret = Common::ThreadUtil::PinToCpu(receive_thread, cpu); ret != 0) {
                    g_log.warn("[{}] Failed to set CPU affinity to core {}: {}", tag(), cpu, std::strerror(ret));
                } else {
                    g_log.trace("[{}] Receive thread pinned to CPU {}", tag(), cpu);
                }
            }
        }


        // Receive thread main loop.
        void receive_loop() {
            g_log.trace("[{}] Receive thread started", tag());

            std::vector<std::uint8_t> buf(kReadBufferSize);

            while (receive_running.load(std::memory_order_acquire)) {
                std::error_code ec;
                const auto n = tcp_client->ReadSome(buf.data(), buf.size(), ec);

                if (ec) {
                    if (receive_running.load(std::memory_order_relaxed)) {
                        g_log.error("[{}] Receive thread: read error: {}", tag(), ec.message());
                        fault.store(true, std::memory_order_release);
                        report_error(ec, "receive thread read error");
                        set_state(ConnectionState::kError);
                    }
                    break;
                }

                if (n > 0) {
                    stats.bytes_received.fetch_add(n, std::memory_order_relaxed);
                    frame_receiver->Feed(buf.data(), n);
                }
            }

            g_log.trace("[{}] Receive thread stopped", tag());
        }


        // Neither LMDscandatacfg nor the TSC* NTP variables have a documented
        // readback command, so the FIRST streamed telegram is the authoritative
        // verification: it shows exactly which channels the device delivers and
        // what its clock says. Failures go through report_error so the App layer
        // (and the GUI health machine) sees them.
        void verify_scan_content(const ScanData &scan) {
            const auto has16 = [&scan](ChannelContent16 want) {
                for (const auto &ch: scan.channels_16bit) {
                    if (ch.content == want) {
                        return true;
                    }
                }
                return false;
            };

            // Expected content per BuildScanDataConfig: remission is RSSI1 or
            // REFL1 depending on the unit flag; the timestamp block is always on.
            const bool want_remission = config.scan.enable_rssi || config.scan.enable_reflectance;
            std::string missing;
            const auto expect = [&missing](bool want, bool have, const char *name) {
                if (want && !have) {
                    missing += std::string(missing.empty() ? "" : ", ") + name;
                }
            };
            expect(config.scan.enable_distance, has16(ChannelContent16::kDist1), "DIST1");
            expect(want_remission,
                   has16(config.scan.enable_reflectance ? ChannelContent16::kRefl1 : ChannelContent16::kRssi1),
                   config.scan.enable_reflectance ? "REFL1" : "RSSI1");
            expect(config.scan.enable_angle_correction, has16(ChannelContent16::kAngl1), "ANGL1");
            expect(config.scan.enable_quality, !scan.channels_8bit.empty(), "QLTY1");
            expect(true, scan.has_timestamp, "timestamp block");
            if (!missing.empty()) {
                g_log.error("[{}] First scan is missing configured content: {} — LMDscandatacfg did not take "
                            "effect on the device",
                            tag(), missing);
                fault.store(true, std::memory_order_release);
                report_error(make_error_code(ErrorCode::kInvalidConfig), "scan content mismatch: " + missing);
            } else {
                g_log.trace("[{}] First scan verified: all configured channels present", tag());
            }
        }


        // Single SNTP client query (RFC 4330). A HEALTHY answer is a
        // server-mode packet: Mode 4/5, stratum 1..15 (0 = kiss-of-death /
        // unsynchronized), LI != alarm, non-zero transmit timestamp. On
        // failure `detail` carries the concrete reason.
        bool probe_ntp_server(std::string &detail) {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) {
                detail = std::strerror(errno);
                return false;
            }
            timeval tv{};
            tv.tv_sec = kNtpProbeTimeoutMs / 1000;
            tv.tv_usec = (kNtpProbeTimeoutMs % 1000) * 1000;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(kNtpPort);
            if (::inet_pton(AF_INET, config.ntp.server_ip.c_str(), &addr.sin_addr) != 1) {
                ::close(fd);
                detail = "invalid server IP '" + config.ntp.server_ip + "'";
                return false;
            }
            // connect() binds the peer so recv() only accepts the server's answer
            if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                detail = std::strerror(errno);
                ::close(fd);
                return false;
            }

            std::uint8_t pkt[48] = {};
            pkt[0] = 0x23; // LI=0, VN=4, Mode=3 (client)
            if (::send(fd, pkt, sizeof(pkt), 0) != static_cast<ssize_t>(sizeof(pkt))) {
                detail = std::strerror(errno);
                ::close(fd);
                return false;
            }
            std::uint8_t resp[48];
            const ssize_t n = ::recv(fd, resp, sizeof(resp), 0);
            ::close(fd);
            if (n < static_cast<ssize_t>(sizeof(resp))) {
                detail = n < 0 ? std::string("no response: ") + std::strerror(errno) : "short response";
                return false;
            }

            const int li = resp[0] >> 6;
            const int mode = resp[0] & 0x07;
            const int stratum = resp[1];
            bool tx_nonzero = false;
            for (int i = 40; i < 48; ++i) {
                tx_nonzero = tx_nonzero || resp[i] != 0;
            }
            if (mode != 4 && mode != 5) {
                detail = fmt::format("unexpected mode {}", mode);
                return false;
            }
            if (li == 3) {
                detail = "server reports LI=alarm (unsynchronized)";
                return false;
            }
            if (stratum == 0 || stratum > 15) {
                detail = fmt::format("bad stratum {} (kiss-of-death / unsynchronized)", stratum);
                return false;
            }
            if (!tx_nonzero) {
                detail = "zero transmit timestamp";
                return false;
            }
            detail = fmt::format("stratum {}", stratum);
            return true;
        }


        // Periodic server watch while scanning: a server that dies MID-RUN (or
        // between runs) must not be masked by the device's still-accurate
        // clock. Tolerates transient UDP loss; after kNtpProbeMaxFailures
        // consecutive misses the run is faulted.
        void ntp_watch_loop() {
            g_log.trace("[{}] NTP watch thread started (period {} s)", tag(), config.ntp.check_status_s);
            int failures = 0;
            while (ntp_watch_running.load(std::memory_order_acquire)) {
                // Wait one ntp.check_status_s period before the next probe —
                // but after a FAILED probe retry immediately, so three misses
                // resolve within seconds instead of three periods.
                if (failures == 0) {
                    const auto next = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(config.ntp.check_status_s);
                    while (std::chrono::steady_clock::now() < next) {
                        if (!ntp_watch_running.load(std::memory_order_acquire)) {
                            g_log.trace("[{}] NTP watch thread stopped", tag());
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                }
                std::string detail;
                if (probe_ntp_server(detail)) {
                    if (failures > 0) {
                        g_log.info("[{}] NTP server {} reachable again ({})", tag(), config.ntp.server_ip, detail);
                    }
                    failures = 0;
                    stats.ntp_status.store(DriverStatistics::kNtpOk, std::memory_order_relaxed);
                    continue;
                }
                ++failures;
                if (failures < kNtpProbeMaxFailures) {
                    g_log.warn("[{}] NTP server {} probe failed ({}/{}): {}", tag(), config.ntp.server_ip, failures,
                               kNtpProbeMaxFailures, detail);
                    continue; // immediate retry
                }
                stats.ntp_status.store(DriverStatistics::kNtpUnreachable, std::memory_order_relaxed);
                g_log.error("[{}] NTP server {} unreachable ({} consecutive probes failed: {}) — the device clock "
                            "is free-running",
                            tag(), config.ntp.server_ip, failures, detail);
                fault.store(true, std::memory_order_release);
                report_error(make_error_code(ErrorCode::kInvalidConfig), "NTP server unreachable: " + detail);
                return; // fault latched; the app terminates the run
            }
            g_log.trace("[{}] NTP watch thread stopped", tag());
        }


        // Parse thread main loop.
        void parse_loop() {
            g_log.trace("[{}] Parse thread started", tag());

            bool first_frame = true;

            while (parse_running.load(std::memory_order_acquire)) {
                RawFrame frame;
                if (!ring_buffer->try_pop(frame)) {
                    // No frames available — brief backoff.
                    std::this_thread::sleep_for(kParseBackoffSleep);
                    continue;
                }

                CoLaBMessage msg;
                auto ec = CoLaBCodec::Decode(frame.data.data(), frame.data.size(), msg, tag());
                if (ec) {
                    stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
                    g_log.warn("[{}] Frame decode error: {}", tag(), ec.message());
                    continue;
                }

                // Only process scan data notifications.
                if (msg.command_name != "LMDscandata") {
                    continue;
                }

                ScanData scan;
                ec = ScanDataParser::Parse(msg.payload.data(), msg.payload.size(), scan, tag());
                if (ec) {
                    stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
                    g_log.warn("[{}] Scan data parse error: {}", tag(), ec.message());
                    continue;
                }

                stats.frames_parsed.fetch_add(1, std::memory_order_relaxed);

                if (first_frame) {
                    verify_scan_content(scan);
                    if (config.ntp.enable && !scan.has_timestamp) {
                        // Already faulted as missing content; record why for ntp=
                        stats.ntp_status.store(DriverStatistics::kNtpNoTimestamp, std::memory_order_relaxed);
                    }
                }

                // Check telegram counter continuity.
                if (!first_frame) {
                    const auto prev_counter = stats.last_telegram_counter.load(std::memory_order_relaxed);
                    const auto expected = static_cast<std::uint32_t>((prev_counter + 1) & 0xFFFF);
                    if (scan.telegram_counter != expected) {
                        const auto gap = (scan.telegram_counter >= expected)
                                             ? scan.telegram_counter - expected
                                             : (0x10000 + scan.telegram_counter - expected);
                        stats.counter_gaps.fetch_add(1, std::memory_order_relaxed);
                        g_log.warn("[{}] Telegram counter gap: expected {}, got {} (missed ~{} frames)", tag(),
                                   expected, scan.telegram_counter, gap);
                    }
                }
                first_frame = false;

                stats.last_telegram_counter.store(scan.telegram_counter, std::memory_order_relaxed);
                stats.last_scan_counter.store(scan.scan_counter, std::memory_order_relaxed);
                stats.last_frame_time_us.store(frame.receive_timestamp_us, std::memory_order_relaxed);

                ScanDataCallback cb;
                {
                    std::lock_guard lock(callback_mutex);
                    cb = scan_callback;
                }
                if (cb) {
                    try {
                        cb(scan);
                    } catch (const std::exception &e) {
                        g_log.warn("[{}] Scan callback threw: {}", tag(), e.what());
                    }
                }
            }

            g_log.trace("[{}] Parse thread stopped", tag());
        }
    };


    LMS4xxxDriver::LMS4xxxDriver(const DriverConfig &config) : impl_(std::make_unique<Impl>(config)) {
    }


    LMS4xxxDriver::~LMS4xxxDriver() {
        if (impl_->scanning.load(std::memory_order_relaxed)) {
            StopScanning();
        }
        if (impl_->tcp_client && impl_->tcp_client->IsConnected()) {
            Disconnect();
        }
    }


    std::error_code LMS4xxxDriver::Connect() {
        auto ec = impl_->config.Validate();
        if (ec) {
            return ec;
        }

        impl_->set_state(ConnectionState::kConnecting);
        g_log.info("[{}] Connecting to {}:{}", impl_->tag(), impl_->config.device.ip, impl_->config.device.port);

        impl_->tcp_client = std::make_unique<TCPClient>(MakeTcpOptions(impl_->config.device, impl_->config.network));

        ec = impl_->tcp_client->Connect(impl_->config.network.connect_timeout_ms);
        if (ec) {
            impl_->set_state(ConnectionState::kError);
            impl_->report_error(ec, "TCP connect failed");
            return ec;
        }

        impl_->set_state(ConnectionState::kConnected);
        g_log.info("[{}] Connected", impl_->tag());
        return {};
    }


    std::error_code LMS4xxxDriver::Configure() {
        if (!impl_->tcp_client || !impl_->tcp_client->IsConnected()) {
            return make_error_code(ErrorCode::kNotConnected);
        }

        impl_->set_state(ConnectionState::kConfiguring);
        const int timeout = impl_->config.network.response_timeout_ms;

        // Following the workflow documented on Page 69 of the Operating Instructions
        // 1. Log in as Authorized Client
        {
            auto frame = CommandBuilder::BuildLogin();
            CoLaBMessage response;
            auto ec = impl_->send_and_receive(frame, response, timeout);
            if (ec) {
                g_log.error("[{}] Login failed: {}", impl_->tag(), ec.message());
                impl_->set_state(ConnectionState::kError);
                return ec;
            }
            ec = impl_->validate_response(response, CommandType::kMethodAnswer, "SetAccessMode");
            if (ec) {
                return ec;
            }

            // Check response: payload[0] should be 0x01 (success)
            if (response.payload.empty() || response.payload[0] != 0x01) {
                g_log.error("[{}] Login rejected by device", impl_->tag());
                impl_->set_state(ConnectionState::kError);
                return make_error_code(ErrorCode::kAccessDenied);
            }
            g_log.trace("[{}] Logged in as Authorized Client", impl_->tag());
        }


        // 2. Configure scandata content. LMDscandatacfg has no readback command;
        // the delivered channel set is verified against the FIRST streamed scan
        // (verify_scan_content on the first parsed scan).
        {
            auto frame = CommandBuilder::BuildScanDataConfig(impl_->config.scan);
            CoLaBMessage response;
            auto ec = impl_->send_and_receive(frame, response, timeout);
            if (ec) {
                g_log.error("[{}] LMDscandatacfg write failed: {}", impl_->tag(), ec.message());
                impl_->set_state(ConnectionState::kError);
                return ec;
            }
            ec = impl_->validate_response(response, CommandType::kWriteAnswer, "LMDscandatacfg");
            if (ec) {
                return ec;
            }
            g_log.trace("[{}] LMDscandatacfg has been set (content verified on the first scan)", impl_->tag());
        }


        // 3. Configure scandata output range (write -> read back -> byte-compare)
        {
            auto ec = impl_->write_and_verify("LMPoutputRange",
                                              CommandBuilder::BuildOutputRange(impl_->config.scan), timeout);
            if (ec) {
                impl_->set_state(ConnectionState::kError);
                return ec;
            }
            g_log.trace("[{}] Output range verified: {:.4f}° to {:.4f}° @ {:.4f}° resolution", impl_->tag(),
                        impl_->config.scan.start_angle_deg, impl_->config.scan.stop_angle_deg,
                        impl_->config.scan.angular_resolution_deg);
        }


        // 4. Parameter settings before store parameters
        // 4.1. Always disable all filters (hardcoded)
        {
            auto cmds = {
                CommandBuilder::BuildMeanFilter(false, 2),
                CommandBuilder::BuildMedianFilter(false),
                CommandBuilder::BuildFrontendEdgeFilter(false),
                CommandBuilder::BuildEdgeFilter(false),
                CommandBuilder::BuildCubicAreaFilter(false, 0, 91776, 0, 0),
                CommandBuilder::BuildGlossFilter(false),
            };
            for (const auto &frame: cmds) {
                CoLaBMessage response;
                auto ec = impl_->send_and_receive(frame, response, timeout);
                if (ec) {
                    g_log.warn("[{}] Filter disable command failed: {}", impl_->tag(), ec.message());
                }
            }
            g_log.trace("[{}] All filters disabled", impl_->tag());
        }


        // 4.2. Configure NTP (if enabled). The TSC* variables have no readback
        // command, so acceptance is checked per-write (sWA) and the EFFECT is
        // verified by probing the NTP SERVER itself (probe_ntp_server here,
        // plus the periodic watch while scanning). A recording whose device
        // clock is undisciplined is worthless — every failure here is fatal
        // for Configure(), never silently degraded.
        if (impl_->config.ntp.enable) {
            // Send an NTP sWN command, validate the sWA response.
            auto ntp_write = [&](const std::vector<std::uint8_t> &frame, std::string_view cmd_name) -> std::error_code {
                CoLaBMessage response;
                auto ec = impl_->send_and_receive(frame, response, timeout);
                if (ec) {
                    g_log.error("[{}] {} write failed: {}", impl_->tag(), cmd_name, ec.message());
                    return ec;
                }
                ec = impl_->validate_response(response, CommandType::kWriteAnswer, cmd_name);
                if (ec) {
                    g_log.error("[{}] {} rejected by the device (response: {} {})", impl_->tag(), cmd_name,
                                response.command_type, response.command_name);
                    return ec;
                }
                g_log.trace("[{}] {} has been set (sWA acknowledged)", impl_->tag(), cmd_name);
                return {};
            };

            boost::system::error_code bec;
            const auto addr = boost::asio::ip::make_address_v4(impl_->config.ntp.server_ip, bec);
            if (bec) {
                g_log.error("[{}] Invalid NTP server IP '{}'", impl_->tag(), impl_->config.ntp.server_ip);
                impl_->set_state(ConnectionState::kError);
                return make_error_code(ErrorCode::kInvalidConfig);
            }

            for (const auto &[frame, name]: {
                     std::pair{
                         CommandBuilder::BuildSetTimeSyncRole(static_cast<std::uint8_t>(impl_->config.ntp.role)),
                         std::string_view("TSCRole")
                     },
                     std::pair{
                         CommandBuilder::BuildSetNtpServer(addr.to_bytes()),
                         std::string_view("TSCTCSrvAddr")
                     },
                     std::pair{
                         CommandBuilder::BuildSetNtpUpdateTime(impl_->config.ntp.update_interval_s),
                         std::string_view("TSCTCupdatetime")
                     },
                     // hardcoded COORD_WORLD_TIME (34): device timestamps are UTC
                     std::pair{CommandBuilder::BuildSetNtpTimezone(34), std::string_view("TSCTCtimezone")},
                 }) {
                if (auto ec = ntp_write(frame, name)) {
                    impl_->set_state(ConnectionState::kError);
                    return ec;
                }
            }

            // Effect verification = server liveness (SNTP probe), NOT a device
            // clock comparison: a device synced on a previous run keeps
            // near-correct time and would silently mask a dead server.
            std::string ntp_detail;
            if (!impl_->probe_ntp_server(ntp_detail)) {
                g_log.error("[{}] NTP server {} check failed: {}", impl_->tag(), impl_->config.ntp.server_ip,
                            ntp_detail);
                impl_->set_state(ConnectionState::kError);
                return make_error_code(ErrorCode::kInvalidConfig);
            }
            impl_->stats.ntp_status.store(DriverStatistics::kNtpOk, std::memory_order_relaxed);
            g_log.info("[{}] NTP configured: server={} ({}), interval={} s, timezone=UTC",
                       impl_->tag(), impl_->config.ntp.server_ip, ntp_detail, impl_->config.ntp.update_interval_s);

            // Record the host wall-clock time of successful NTP configuration.
            struct timespec ts{};
            if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
                const auto us = static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000ULL + static_cast<std::uint64_t>(ts.
                                    tv_nsec) / 1'000ULL;
                impl_->stats.ntp_configured_at_us.store(us, std::memory_order_relaxed);
            }
        } else {
            // NTP disabled: set TSCRole=Off. mEEwriteall below will persist it.
            CoLaBMessage response;
            auto ec = impl_->send_and_receive(
                CommandBuilder::BuildSetTimeSyncRole(static_cast<std::uint8_t>(TscRole::kOff)), response, timeout);
            if (ec) {
                g_log.warn("[{}] Failed to disable TSCRole (non-fatal): {}", impl_->tag(), ec.message());
            }
            g_log.trace("[{}] NTP disabled (Timestamp role=Off)", impl_->tag());
        }


        // 5. Store parameters
        {
            auto frame = CommandBuilder::BuildSaveParams();
            CoLaBMessage response;
            auto ec = impl_->send_and_receive(frame, response, timeout);
            if (ec) {
                g_log.warn("[{}] mEEwriteall failed: {} (params activated but not persisted)", impl_->tag(),
                           ec.message());
            } else {
                ec = impl_->validate_response(response, CommandType::kMethodAnswer, "mEEwriteall");
                if (ec) {
                    g_log.warn("[{}] mEEwriteall unexpected response: {} {}", impl_->tag(), response.command_type,
                               response.command_name);
                } else {
                    g_log.trace("[{}] Parameters saved (mEEwriteall)", impl_->tag());
                }
            }
        }


        // 6. Start measurement
        {
            auto frame = CommandBuilder::BuildStartMeasurement();
            CoLaBMessage response;
            auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
            if (ec) {
                g_log.error("[{}] Start measurement command failed: {}", impl_->tag(), ec.message());
                return ec;
            }
            auto validate_ec = impl_->validate_response(response, CommandType::kMethodAnswer, "LMCstartmeas");
            if (validate_ec) {
                return validate_ec;
            }

            if (response.payload.empty() || response.payload[0] != 0x00) {
                g_log.error("[{}] Start measurement rejected", impl_->tag());
                return make_error_code(ErrorCode::kCommandRejected);
            }
        }


        // 7. Activate configuration and log out (sMN Run)
        {
            auto frame = CommandBuilder::BuildRun();
            CoLaBMessage response;
            auto ec = impl_->send_and_receive(frame, response, timeout);
            if (ec) {
                g_log.error("[{}] Run command failed: {}", impl_->tag(), ec.message());
                impl_->set_state(ConnectionState::kError);
                return ec;
            }
            ec = impl_->validate_response(response, CommandType::kMethodAnswer, "Run");
            if (ec) {
                return ec;
            }

            if (response.payload.empty() || response.payload[0] != 0x01) {
                g_log.error("[{}] Run command rejected", impl_->tag());
                impl_->set_state(ConnectionState::kError);
                return make_error_code(ErrorCode::kCommandRejected);
            }
            g_log.trace("[{}] Configuration activated (Run)", impl_->tag());
        }

        impl_->set_state(ConnectionState::kConnected);
        return {};
    }


    std::error_code LMS4xxxDriver::StartScanning() {
        if (!impl_->tcp_client || !impl_->tcp_client->IsConnected()) {
            return make_error_code(ErrorCode::kNotConnected);
        }
        if (impl_->scanning.load(std::memory_order_relaxed)) {
            return make_error_code(ErrorCode::kAlreadyScanning);
        }

        impl_->stats.Reset(); // ntp_status keeps Configure()'s probe result
        impl_->fault.store(false, std::memory_order_release);

        impl_->ring_buffer = std::make_unique<FrameRingBuffer>(impl_->config.network.ring_buffer_frames);

        auto *stats_ptr = &impl_->stats;
        auto *ring_ptr = impl_->ring_buffer.get();

        impl_->frame_receiver = std::make_unique<FrameReceiver>(
            // on_frame: push to ring buffer
            [stats_ptr, ring_ptr](RawFrame &&frame) {
                stats_ptr->frames_received.fetch_add(1, std::memory_order_relaxed);
                if (!ring_ptr->try_push(std::move(frame))) {
                    stats_ptr->frames_dropped.fetch_add(1, std::memory_order_relaxed);
                }
            },
            // on_error: count errors
            [stats_ptr](const char *reason) {
                // Distinguish CRC from framing errors by keyword.
                if (std::strstr(reason, "CRC") || std::strstr(reason, "checksum")) {
                    stats_ptr->crc_errors.fetch_add(1, std::memory_order_relaxed);
                } else {
                    stats_ptr->framing_errors.fetch_add(1, std::memory_order_relaxed);
                }
            },
            64 * 1024, impl_->tag());

        {
            auto frame = CommandBuilder::BuildStartStream();
            CoLaBMessage response;
            auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
            if (ec) {
                g_log.error("[{}] Start stream command failed: {}", impl_->tag(), ec.message());
                return ec;
            }
            auto validate_ec = impl_->validate_response(response, CommandType::kEventAnswer, "LMDscandata");
            if (validate_ec) {
                return validate_ec;
            }

            if (response.payload.empty() || response.payload[0] != 0x01) {
                g_log.error("[{}] Start stream rejected", impl_->tag());
                return make_error_code(ErrorCode::kCommandRejected);
            }
        }

        impl_->scanning.store(true, std::memory_order_release);
        impl_->receive_running.store(true, std::memory_order_release);
        impl_->parse_running.store(true, std::memory_order_release);

        impl_->parse_thread = std::thread([this]() { impl_->parse_loop(); });
        impl_->receive_thread = std::thread([this]() { impl_->receive_loop(); });
        if (impl_->config.ntp.enable) {
            impl_->ntp_watch_running.store(true, std::memory_order_release);
            impl_->ntp_watch_thread = std::thread([this]() { impl_->ntp_watch_loop(); });
        }

        // Configure receive thread for real-time scheduling (after thread creation)
        impl_->configure_receive_thread();

        impl_->set_state(ConnectionState::kScanning);
        g_log.trace("[{}] Scanning started (ring buffer capacity: {} frames)", impl_->tag(),
                    impl_->ring_buffer->capacity());
        return {};
    }


    std::error_code LMS4xxxDriver::StopScanning() {
        if (!impl_->scanning.load(std::memory_order_relaxed)) {
            return make_error_code(ErrorCode::kNotScanning);
        }

        g_log.trace("[{}] Stopping scan...", impl_->tag());

        impl_->receive_running.store(false, std::memory_order_release);
        impl_->parse_running.store(false, std::memory_order_release);
        impl_->scanning.store(false, std::memory_order_release);

        // Send stop stream command BEFORE shutting down receive.
        // Full-duplex guarantees concurrent Write + ReadSome is safe.
        if (impl_->tcp_client && impl_->tcp_client->IsConnected()) {
            auto frame = CommandBuilder::BuildStopStream();
            auto ec = impl_->tcp_client->Write(frame);
            if (ec) {
                g_log.warn("[{}] Failed to send stop stream command (non-fatal): {}", impl_->tag(), ec.message());
            }
            frame = CommandBuilder::BuildStandby();
            ec = impl_->tcp_client->Write(frame);
            if (ec) {
                g_log.warn("[{}] Failed to send standby command (non-fatal): {}", impl_->tag(), ec.message());
            }
        }

        // Unblock the receive thread: Boost.Asio uses epoll_wait() internally for
        // synchronous read_some(), so SO_RCVTIMEO has no effect. Calling
        // shutdown(SHUT_RD) triggers POLLHUP which wakes epoll_wait() immediately,
        // causing read_some() to return EOF. The receive loop will then see
        // receive_running==false and exit cleanly.
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
        if (impl_->parse_thread.joinable()) {
            impl_->parse_thread.join();
        }

        impl_->set_state(ConnectionState::kConnected);
        g_log.trace("[{}] Scanning stopped", impl_->tag());
        return {};
    }


    void LMS4xxxDriver::Disconnect() {
        if (impl_->scanning.load(std::memory_order_relaxed)) {
            StopScanning();
        }

        if (impl_->tcp_client) {
            impl_->tcp_client->Disconnect();
        }

        impl_->frame_receiver.reset();
        impl_->ring_buffer.reset();

        impl_->set_state(ConnectionState::kDisconnected);
        g_log.trace("[{}] Disconnected", impl_->tag());
    }


    void LMS4xxxDriver::SetScanCallback(ScanDataCallback callback) {
        std::lock_guard lock(impl_->callback_mutex);
        impl_->scan_callback = std::move(callback);
    }


    void LMS4xxxDriver::SetErrorCallback(ErrorCallback callback) {
        std::lock_guard lock(impl_->callback_mutex);
        impl_->error_callback = std::move(callback);
    }


    ConnectionState LMS4xxxDriver::GetConnectionState() const {
        return impl_->state.load(std::memory_order_acquire);
    }


    bool LMS4xxxDriver::IsConnected() const {
        auto s = impl_->state.load(std::memory_order_acquire);
        return s != ConnectionState::kDisconnected && s != ConnectionState::kError;
    }


    bool LMS4xxxDriver::IsScanning() const {
        return impl_->scanning.load(std::memory_order_acquire);
    }


    bool LMS4xxxDriver::HasFault() const {
        return impl_->fault.load(std::memory_order_acquire);
    }


    DriverStatistics::Snapshot LMS4xxxDriver::GetStatistics() const {
        return impl_->stats.GetSnapshot();
    }


    std::error_code LMS4xxxDriver::PollSingleScan(ScanData &out) {
        if (impl_->scanning.load(std::memory_order_relaxed)) {
            return make_error_code(ErrorCode::kAlreadyScanning);
        }

        auto frame = CommandBuilder::BuildPollScan();
        CoLaBMessage response;
        auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
        if (ec) {
            return ec;
        }

        ec = impl_->validate_response(response, CommandType::kReadAnswer, "LMDscandata");
        if (ec) {
            return ec;
        }

        return ScanDataParser::Parse(response.payload.data(), response.payload.size(), out, impl_->tag());
    }


    std::error_code LMS4xxxDriver::StartMeasurement() {
        // Requires login first. Caller should call configure() which includes login.
        auto frame = CommandBuilder::BuildStartMeasurement();
        CoLaBMessage response;
        auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
        if (ec) {
            return ec;
        }

        ec = impl_->validate_response(response, CommandType::kMethodAnswer, "LMCstartmeas");
        if (ec) {
            return ec;
        }

        if (response.payload.empty() || response.payload[0] != 0x00) {
            return make_error_code(ErrorCode::kCommandRejected);
        }
        g_log.trace("[{}] Measurement started", impl_->tag());
        return {};
    }


    std::error_code LMS4xxxDriver::StopMeasurement() {
        auto frame = CommandBuilder::BuildStopMeasurement();
        CoLaBMessage response;
        auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
        if (ec) {
            return ec;
        }

        ec = impl_->validate_response(response, CommandType::kMethodAnswer, "LMCstopmeas");
        if (ec) {
            return ec;
        }

        if (response.payload.empty() || response.payload[0] != 0x00) {
            return make_error_code(ErrorCode::kCommandRejected);
        }
        g_log.trace("[{}] Measurement stopped", impl_->tag());
        return {};
    }


    std::error_code LMS4xxxDriver::Standby() {
        auto frame = CommandBuilder::BuildStandby();
        CoLaBMessage response;
        auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
        if (ec) {
            return ec;
        }

        ec = impl_->validate_response(response, CommandType::kMethodAnswer, "LMCstandby");
        if (ec) {
            return ec;
        }

        if (response.payload.empty() || response.payload[0] != 0x00) {
            return make_error_code(ErrorCode::kCommandRejected);
        }
        g_log.info("[{}] Entered standby mode", impl_->tag());
        return {};
    }


    std::error_code LMS4xxxDriver::RebootDevice() {
        auto frame = CommandBuilder::BuildReboot();
        CoLaBMessage response;
        auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
        if (ec) {
            return ec;
        }

        g_log.info("[{}] Reboot command sent", impl_->tag());
        return {};
    }


    void LMS4xxxDriver::SetScanConfig(const ScanConfig &config) {
        impl_->config.scan = config;
    }

    const DriverConfig &LMS4xxxDriver::GetConfig() const {
        return impl_->config;
    }


    std::error_code DriverConfig::Validate() const {
        // Runs before LMS4xxxDriver exists, so resolve the log tag locally
        const std::string &vtag = name.empty() ? device.ip : name;
        if (device.ip.empty()) {
            g_log.error("[{}] Device IP is empty", vtag);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (device.port == 0) {
            g_log.error("[{}] Device port is 0", vtag);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (scan.start_angle_deg >= scan.stop_angle_deg) {
            g_log.error("[{}] Start angle ({}) must be less than stop angle ({})", vtag, scan.start_angle_deg,
                        scan.stop_angle_deg);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (scan.angular_resolution_deg <= 0.0) {
            g_log.error("[{}] Angular resolution must be positive", vtag);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (scan.output_rate == 0) {
            g_log.error("[{}] Output rate must be >= 1", vtag);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (scan.enable_rssi && scan.enable_reflectance) {
            // LMDscandatacfg carries ONE remission channel (the unit flag picks
            // RSSI1 or REFL1); with both enabled the writer would record a
            // permanently zero-filled RSSI1 array while claiming it holds data.
            g_log.error("[{}] enable_rssi and enable_reflectance are mutually exclusive (the device streams "
                        "one remission channel)",
                        vtag);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (network.ring_buffer_frames < 16) {
            g_log.error("[{}] Ring buffer too small: {} (minimum 16)", vtag, network.ring_buffer_frames);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (ntp.enable && ntp.server_ip.empty()) {
            g_log.error("[{}] NTP enabled but server IP is empty", vtag);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (ntp.enable && (ntp.check_status_s < 1 || ntp.check_status_s > 3600)) {
            g_log.error("[{}] NTP check_status_s {} s is outside the valid range [1, 3600]", vtag,
                        ntp.check_status_s);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        if (ntp.enable && (ntp.update_interval_s < 1 || ntp.update_interval_s > 3600)) {
            // Manual: TSCTCupdatetime valid range 1..3600 s; out-of-range values
            // would only surface as a device rejection mid-Configure
            g_log.error("[{}] NTP update interval {} s is outside the device's valid range [1, 3600]", vtag,
                        ntp.update_interval_s);
            return make_error_code(ErrorCode::kInvalidConfig);
        }
        return {};
    }
} // namespace LMS4xxx
