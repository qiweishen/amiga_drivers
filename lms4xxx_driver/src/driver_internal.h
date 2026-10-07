#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "driver.h"
#include "clock_quality.h"
#include "command_channel.h"
#include "frame_receiver.h"
#include "ring_buffer.h"

namespace lms4xxx {
    using FrameRingBuffer = common::RingBuffer<RawFrame>;

    // Private shared state. Lifecycle owns connections/threads; configuration
    // runs before stream ownership transfers to receive/parse. The only
    // cross-thread control replies and telemetry are guarded by telemetry_mutex.
    struct Lms4xxxDriver::Impl {
        DriverConfig config;
        CommandChannel commands;

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
        MeasurementShutdown measurement_shutdown; // independent of the host stream/threads
        bool stream_start_requested = false; // owner thread; the reply may be lost

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


        explicit Impl(const DriverConfig &cfg);
        [[nodiscard]] const std::string &Tag() const { return config.name; }
        void SetState(ConnectionState new_state) { state.store(new_state, std::memory_order_release); }
        void ReportError(std::error_code ec, const std::string &detail = "");

        // Command-phase configuration and readback (driver_configuration.cpp).
        enum class Readback { kStrict, kTolerateSfa };

        struct ParamRow {
            std::string_view name;
            std::vector<std::uint8_t> frame;
            Readback readback;
            const char *why;
        };
        static bool IsTolerableSfa(const std::error_code &ec);
        std::error_code ApplyParameter(const ParamRow &row, int timeout_ms, int &changed);
        static std::string PrefixedString(const std::vector<std::uint8_t> &p);
        void ReadIdentity(int timeout_ms);
        static std::vector<std::string> DecodeActiveMessages(const std::vector<std::uint8_t> &p,
                                                              bool *complete = nullptr);
        void LogActiveMessages(int timeout_ms);
        void ReadAudit(int timeout_ms);
        std::error_code WaitDeviceReady(int timeout_ms);

        // Worker execution and streaming control replies (driver_stream.cpp).
        void RequestTelemetry();
        void HandleNonScanFrame(const ColaBMessage &msg);
        bool CallMethodStreaming(const std::vector<std::uint8_t> &frame, const std::string &name,
                                 int expected_status, int timeout_ms);
        bool TakeTelemetry(TelemetrySample &out);
        void ConfigureReceiveThread();
        void ReceiveLoop();
        void VerifyFirstScan(const ScanData &scan);
        bool ProbeNtpServer(std::string &detail) const;
        void NtpWatchLoop();
        struct TimeGate {
            std::chrono::steady_clock::time_point deadline;
            bool plausible_seen = false;
            bool anomaly_seen = false;
            ClockQualityTracker tracker;
        };
        void AssessScanTime(ScanData &scan, TimeGate &gate);
        void ParseLoop();

        // Lifecycle cleanup (driver.cpp); reconnect requires all workers joined.
        std::error_code ParkMeasurement(bool streaming, bool reconnect);
    };
} // namespace lms4xxx
