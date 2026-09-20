#include "lms4xxx_driver_app.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <thread>

#include "driver_markers.h"
#include "logger.h"
#include "time_util.h"
#include "utility.h"


namespace {
    common::DriverLog g_log{std::string(common::Markers::kModuleLms4xxx)};

    constexpr auto kPollInterval = std::chrono::milliseconds(200);

    // DriverStatistics::NtpStatus -> status-line token
    const char *NtpStatusText(lms4xxx::DriverStatistics::NtpStatus status) {
        switch (status) {
            case lms4xxx::DriverStatistics::NtpStatus::kUnverified:
                return "UNVERIFIED";
            case lms4xxx::DriverStatistics::NtpStatus::kNoSignal:
                return "NO-SIGNAL";
            case lms4xxx::DriverStatistics::NtpStatus::kStale:
                return "UNKNOWN";
            case lms4xxx::DriverStatistics::NtpStatus::kClockAnomaly:
                return "TIME-ANOMALY";
            case lms4xxx::DriverStatistics::NtpStatus::kNoTimestamp:
                return "NO-TS";
            case lms4xxx::DriverStatistics::NtpStatus::kUnreachable:
                return "UNREACH";
            case lms4xxx::DriverStatistics::NtpStatus::kNotLocked:
                return "NO-LOCK";
            default:
                return "OFF";
        }
    }

    // Fail-fast: the name of the first counter that proves the recording is no
    // longer complete, or nullptr. Same predicate as the final verdict in Shutdown().
    const char *FirstLoss(const lms4xxx::DriverStatistics::Snapshot &drv,
                          const lms4xxx::ScanRecordWriter::Statistics &wr) {
        if (drv.frames_dropped != 0) return "receive ring overflow";
        if (wr.frames_dropped != 0) return "writer queue overflow";
        if (drv.counter_gaps != 0) return "telegram counter gap";
        if (drv.crc_errors != 0) return "frame checksum error";
        if (drv.framing_errors != 0) return "framing error";
        if (drv.parse_errors != 0) return "parse error";
        return nullptr;
    }

    // DIST + ANGL + QLTY always; the remission channel follows scan.remission
    std::uint16_t BuildChannelMask(const lms4xxx::ScanConfig &scan) {
        const std::uint16_t remission = scan.remission == lms4xxx::Remission::kRefl
                                            ? lms4xxx::ChannelMask::kRefl1
                                            : lms4xxx::ChannelMask::kRssi1;
        return static_cast<std::uint16_t>(lms4xxx::ChannelMask::kDist1 | remission | lms4xxx::ChannelMask::kAngl1 |
                                          lms4xxx::ChannelMask::kQlty1);
    }
} // namespace


Lms4xxxDriverApp::Lms4xxxDriverApp(const common::Config &run, const lms4xxx::AppConfig &config,
                                   const lms4xxx::LidarConfig &lidar) :
    impl_(std::make_unique<Impl>()),
    instance_name_(lidar.id),
    driver_config_(config.DriverConfigFor(lidar)),
    output_(config.output),
    telemetry_interval_s_(config.telemetry_interval_s),
    stats_interval_s_(config.stats_interval_s),
    data_folder_path_(run.data_folder_path.string()),
    timestamp_(run.timestamp),
    config_path_(run.lms4xxx_config_path.string()) {
}


Lms4xxxDriverApp::~Lms4xxxDriverApp() {
    if (impl_->driver) {
        Shutdown();
    }
}


bool Lms4xxxDriverApp::Init(const std::function<bool()> &external_stop) {
    const auto stopped = [&] {
        return terminate_.load(std::memory_order_acquire) || (external_stop && external_stop());
    };
    if (stopped()) {
        return false;
    }
    impl_->driver = std::make_unique<lms4xxx::Lms4xxxDriver>(driver_config_);

    impl_->driver->SetErrorCallback([name = instance_name_](std::error_code err, const std::string &detail) {
        g_log.Error("[{}] Error: {}{}", name, err.message(), detail.empty() ? "" : " (" + detail + ")");
    });

    if (data_folder_path_.empty()) {
        g_log.Error("[{}] Cannot set up the scan writer (empty data folder path)", instance_name_);
        return false;
    }

    if (!driver_config_.ntp.enabled) {
        // Host time is never recorded on this platform; NTP is the LiDAR's only absolute time
        g_log.Warn("[{}] ntp.enabled=false: scans carry NO absolute time (device uptime only, wraps every "
                   "71.6 min); offline time association with the other sensors is impossible for this run",
                   instance_name_);
    }

    auto ec = impl_->driver->Connect();
    if (ec) {
        g_log.Error("[{}] Connect failed: {}", instance_name_, ec.message());
        return false;
    }

    if (stopped()) {
        return false;
    }
    ec = impl_->driver->Configure();
    if (ec) {
        g_log.Error("[{}] Configure failed: {}", instance_name_, ec.message());
        impl_->driver->Disconnect();
        return false;
    }

    if (stopped()) {
        return false;
    }
    // The writer needs the device identity, so it is created after Configure()
    const auto &identity = impl_->driver->GetDeviceIdentity();
    lms4xxx::ScanRecordWriter::Config writer_config;
    writer_config.output_path = fmt::format("{}/lms4xxx/scan_{}_{}.h5", data_folder_path_, instance_name_, timestamp_);
    writer_config.instance = instance_name_;
    writer_config.session_timestamp = timestamp_;
    writer_config.config_yaml_path = config_path_;
    writer_config.channel_mask = BuildChannelMask(driver_config_.scan);
    writer_config.remission = lms4xxx::RemissionName(driver_config_.scan.remission);
    writer_config.device_firmware = identity.firmware_designation +
                                    (identity.firmware_version.empty() ? "" : " " + identity.firmware_version);
    writer_config.device_order_number = identity.order_number;
    writer_config.device_type = identity.device_type;
    writer_config.device_name = identity.location_name;
    writer_config.device_keeps_flagged_points = identity.is_s01_variant;
    writer_config.audit = impl_->driver->GetDeviceAudit(); // -> root attributes (docs/FORMAT_H5.md)
    writer_config.queue_capacity = output_.queue_max_frames;
    writer_config.max_file_bytes = output_.max_file_bytes;
    writer_config.chunk_frames = output_.chunk_frames;
    writer_config.flush_interval_ms = output_.flush_interval_ms;
    writer_config.compression_level = output_.compression_level;
    writer_config.swmr = output_.swmr;

    impl_->writer = std::make_unique<lms4xxx::ScanRecordWriter>(writer_config);
    impl_->driver->SetScanCallback([writer = impl_->writer.get()](const lms4xxx::ScanData &scan) {
        writer->OnScan(scan);
    });
    g_log.Trace("[{}] Scan recording enabled (channels: 0x{:02X})", instance_name_,
                writer_config.channel_mask);

    g_log.Info(fmt::runtime(common::Markers::kLmsInitializedInstTpl), instance_name_);
    return true;
}


void Lms4xxxDriverApp::Run() {
    if (!impl_->driver) {
        g_log.Error(true, "Run() called without Init()");
        return;
    }

    // Before scanning so no frames are missed
    if (impl_->writer && !impl_->writer->Start()) {
        terminate_.store(true, std::memory_order_release);
        g_log.Error(true, "[{}] Cannot start the scan writer", instance_name_);
        return; // unreachable
    }

    auto ec = impl_->driver->StartScanning();
    if (ec) {
        // Error(true, ...) never returns
        if (impl_->writer) {
            impl_->writer->Stop();
        }
        terminate_.store(true, std::memory_order_release);
        g_log.Error(true, "[{}] Start scanning failed: {}", instance_name_, ec.message());
        return; // unreachable; keeps the control flow obvious
    }

    g_log.Info("[{}] Scanning started", instance_name_);

    using clock = std::chrono::steady_clock;
    const double stats_interval = stats_interval_s_;
    impl_->scan_start = clock::now();
    auto last_stats = impl_->scan_start;
    auto last_telemetry_request = impl_->scan_start;
    std::uint64_t rate_prev_frames = 0;
    std::uint64_t rate_prev_written = 0;
    std::uint64_t rate_prev_queued = 0;
    auto rate_prev_time = impl_->scan_start;

    // Arms Main's no-data watchdog: from here on, silence is not expected.
    // A device that keeps the TCP connection open but stops streaming produces
    // no read error, no fault and no frames, so nothing else would end the run.
    scan_start_us_.store(common::TimeUtil::SteadyNowUs(), std::memory_order_release);

    // One telemetry round immediately, so even a short run carries a sample.
    if (telemetry_interval_s_ > 0.0) {
        impl_->driver->RequestTelemetry();
    }

    while (!terminate_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kPollInterval);

        if (!impl_->driver->IsScanning()) {
            g_log.Warn("[{}] Scanning stopped unexpectedly", instance_name_);
            RequestFailure();
            break;
        }
        // Data past a fault is invalid: end the Run (cause already logged)
        if (impl_->driver->HasFault()) {
            MarkFailed();
            g_log.Error("[{}] Fatal driver fault — terminating the Run (see the error above for the cause)",
                        instance_name_);
            terminate_.store(true, std::memory_order_release);
            break;
        }
        // Recording is mandatory: a dead writer ends the run like a failed Start()
        if (impl_->writer && impl_->writer->HasFailed()) {
            MarkFailed();
            g_log.Error("[{}] Scan writer stopped on a recording failure — terminating the Run (see the "
                        "[Writer] error above for the cause)", instance_name_);
            terminate_.store(true, std::memory_order_release);
            break;
        }
        // Fail-fast: the first lost or damaged scan ends the whole rig now rather
        // than marking the session failed at the end — an incomplete recording is
        // worthless and the operator wants to restart immediately
        {
            const auto drv = impl_->driver->GetStatistics();
            const lms4xxx::ScanRecordWriter::Statistics wr =
                    impl_->writer ? impl_->writer->GetStatistics() : lms4xxx::ScanRecordWriter::Statistics{};
            if (const char *what = FirstLoss(drv, wr); what != nullptr) {
                MarkFailed();
                g_log.Error("[{}] First data-loss event ({}) — terminating the run (fail-fast: the recording "
                            "would be incomplete; see the warning above for the details)", instance_name_, what);
                terminate_.store(true, std::memory_order_release);
                break;
            }
        }

        const auto now = clock::now();

        // The stall watchdog lives in Main now (Guards: in config-main.yaml),
        // which polls MicrosSinceLastData() for every sensor with one policy.

        // Device telemetry: ask, then hand whatever came back to the recorder.
        if (telemetry_interval_s_ > 0.0) {
            if (std::chrono::duration<double>(now - last_telemetry_request).count() >=
                telemetry_interval_s_) {
                last_telemetry_request = now;
                impl_->driver->RequestTelemetry();
            }
            lms4xxx::TelemetrySample sample;
            if (impl_->driver->TakeTelemetry(sample) && impl_->writer) {
                impl_->writer->OnTelemetry(sample);
                impl_->last_telemetry = sample;
            }
        }

        if (stats_interval > 0 && std::chrono::duration<double>(now - last_stats).count() >= stats_interval) {
            last_stats = now;
            const auto drv = impl_->driver->GetStatistics();
            const lms4xxx::ScanRecordWriter::Statistics wr =
                    impl_->writer ? impl_->writer->GetStatistics() : lms4xxx::ScanRecordWriter::Statistics{};
            // One CoLa B frame per scan
            const double dt = std::chrono::duration<double>(now - rate_prev_time).count();
            const double scan_hz = dt > 0.0
                                       ? static_cast<double>(drv.frames_received - rate_prev_frames) / dt
                                       : 0.0;
            // app/services/driver_stats.py defines fps= as frames ACTUALLY
            // WRITTEN TO DISK per second, and that is what the dashboard card
            // shows for every other driver. Queue acceptance is a different
            // number and gets its own field.
            const double write_fps = dt > 0.0
                                         ? static_cast<double>(wr.frames_written - rate_prev_written) / dt
                                         : 0.0;
            const double queued_fps = dt > 0.0
                                          ? static_cast<double>(wr.frames_queued - rate_prev_queued) / dt
                                          : 0.0;
            rate_prev_frames = drv.frames_received;
            rate_prev_written = wr.frames_written;
            rate_prev_queued = wr.frames_queued;
            rate_prev_time = now;
            const auto uptime_s = static_cast<std::uint64_t>(std::chrono::duration<double>(now - impl_->scan_start).
                count());
            // Field order and the double-space separator are a GUI contract
            // (app/services/driver_stats.py routes on the "[Statistics] [<id>] "
            // prefix and reads fps=; tools/check_contracts.py pins a rendered
            // sample). New fields are APPENDED, never interleaved.
            g_log.Info("[Statistics] [{}] up={}  rate={:.1f} Hz  fps={:.1f}  ntp={}  frames={}  parsed={}  "
                       "drop_ring={}  gaps={}  crc={}  frame_err={}  parse_err={}  written={}  drop_q={}  files={}  "
                       "bytes={}  queued={:.1f}  temp={}  unexpected={}  prelock={}  tstep_max_us={}  "
                       "utc_back={}  utc_repeat={}  ntp_device_loss={}  ntp_server_reachable={}",
                       instance_name_, common::TimeUtil::HumanDuration(uptime_s), scan_hz, write_fps,
                       NtpStatusText(drv.ntp_status),
                       drv.frames_received, drv.frames_parsed, drv.frames_dropped, drv.counter_gaps, drv.crc_errors,
                       drv.framing_errors, drv.parse_errors, wr.frames_written, wr.frames_dropped, wr.files_created,
                       common::HumanBytes(wr.bytes_written), queued_fps,
                       impl_->last_telemetry.temperature_c
                           ? fmt::format("{:.1f}", *impl_->last_telemetry.temperature_c)
                           : "n/a",
                       drv.unexpected_replies, drv.prelock_scans_discarded, drv.max_time_step_us,
                       drv.utc_backwards, drv.utc_repeated, drv.device_no_ntp_events, drv.ntp_server_reachable);
        }
    }

    // Disarm before the run thread exits: whatever ends the run, silence from
    // here on is expected and must not be reported as a watchdog trip.
    scan_start_us_.store(0, std::memory_order_release);

    g_log.Trace("[{}] Run() exiting", instance_name_);
}


std::optional<std::uint64_t> Lms4xxxDriverApp::MicrosSinceLastData() const {
    const auto started_us = scan_start_us_.load(std::memory_order_acquire);
    if (started_us == 0) {
        return std::nullopt; // not streaming: connecting, configuring, or done
    }
    if (const auto since_frame_us = impl_->driver->MicrosSinceLastFrame(); since_frame_us > 0) {
        return since_frame_us;
    }
    // Nothing has arrived yet, so the subscription itself is the reference.
    const auto now_us = common::TimeUtil::SteadyNowUs();
    return now_us > started_us ? now_us - started_us : 0;
}


void Lms4xxxDriverApp::Shutdown() {
    scan_start_us_.store(0, std::memory_order_release);
    if (!impl_->driver) {
        return;
    }

    if (impl_->driver->IsScanning()) {
        impl_->driver->StopScanning();
    }
    if (impl_->driver->HasFault()) {
        MarkFailed();
    }

    // Stop the writer first so the summary reports the on-disk totals
    if (impl_->writer) {
        impl_->writer->Stop();
        if (impl_->writer->HasFailed()) {
            MarkFailed();
        }
    }

    const auto drv = impl_->driver->GetStatistics();
    const lms4xxx::ScanRecordWriter::Statistics wr =
            impl_->writer ? impl_->writer->GetStatistics() : lms4xxx::ScanRecordWriter::Statistics{};
    // A clean file close does not make missing scans complete. Keep transport,
    // parser and writer loss visible in the rig's final manifest/exit status.
    if (drv.frames_dropped != 0 || drv.counter_gaps != 0 || drv.crc_errors != 0 ||
        drv.framing_errors != 0 || drv.parse_errors != 0 || wr.frames_dropped != 0 ||
        drv.utc_backwards != 0 || drv.clock_step_events != 0 || drv.device_no_ntp_events != 0) {
        MarkFailed();
    }
    final_statistics_ = {
        {"instance", instance_name_}, {"frames_received", drv.frames_received},
        {"frames_parsed", drv.frames_parsed}, {"dropped_ring", drv.frames_dropped},
        {"counter_gaps", drv.counter_gaps}, {"crc_errors", drv.crc_errors},
        {"framing_errors", drv.framing_errors}, {"parse_errors", drv.parse_errors},
        {"frames_queued", wr.frames_queued}, {"frames_written", wr.frames_written},
        {"dropped_writer", wr.frames_dropped}, {"bytes_written", wr.bytes_written},
        {"files_created", wr.files_created}, {"ntp_status", NtpStatusText(drv.ntp_status)},
        {"prelock_scans_discarded", drv.prelock_scans_discarded}, {"max_time_step_us", drv.max_time_step_us},
        {"utc_backwards", drv.utc_backwards}, {"utc_repeated", drv.utc_repeated},
        {"clock_step_events", drv.clock_step_events}, {"device_no_ntp_events", drv.device_no_ntp_events},
        {"ntp_server_reachable", drv.ntp_server_reachable}, {"absolute_time_verified", false},
        {"recording_incomplete", HasFailed()}
    };
    const auto duration_s = impl_->scan_start == std::chrono::steady_clock::time_point{}
                                ? 0ull
                                : static_cast<std::uint64_t>(std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - impl_->scan_start)
                                    .count());
    g_log.Info("[Statistics] [{}] Final: duration={}  frames={}  parsed={}  delivery={:.1f}%  ntp={}  "
               "dropped_ring={}  counter_gaps={}  crc_errors={}  framing_errors={}  parse_errors={}  "
               "unexpected_replies={}  frames_written={}  dropped_queue={}  bytes={}  files={}  prelock={}  "
               "tstep_max_us={}  utc_back={}  utc_repeat={}  clock_step_events={}  ntp_device_loss={}  "
               "ntp_server_reachable={}",
               instance_name_, common::TimeUtil::HumanDuration(duration_s), drv.frames_received, drv.frames_parsed,
               drv.DeliveryRate(), NtpStatusText(drv.ntp_status), drv.frames_dropped, drv.counter_gaps,
               drv.crc_errors, drv.framing_errors, drv.parse_errors, drv.unexpected_replies,
               wr.frames_written, wr.frames_dropped, common::HumanBytes(wr.bytes_written), wr.files_created,
               drv.prelock_scans_discarded, drv.max_time_step_us, drv.utc_backwards, drv.utc_repeated,
               drv.clock_step_events, drv.device_no_ntp_events, drv.ntp_server_reachable);

    impl_->driver->Disconnect();

    if (HasFailed()) {
        g_log.Error("[{}] LMS4xxx driver ended with issues; recording is INCOMPLETE", instance_name_);
    } else {
        g_log.Info(fmt::runtime(common::Markers::kLmsShutdownInstTpl), instance_name_);
    }

    // Makes the destructor and repeated Shutdown() no-ops
    impl_->driver.reset();
}
