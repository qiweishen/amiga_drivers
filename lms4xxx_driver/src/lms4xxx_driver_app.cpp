#include "lms4xxx_driver_app.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "driver_markers.h"
#include "logger.h"
#include "recording_status.h"
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

    nlohmann::ordered_json DeviceMetadata(const lms4xxx::Lms4xxxDriver &driver,
                                          const lms4xxx::DriverConfig &config, const std::string &path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Cannot read session configuration snapshot: " + path);
        const std::string yaml{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto &id = driver.GetDeviceIdentity();
        nlohmann::ordered_json metadata = {
            {"device_firmware", id.firmware_designation + " " + id.firmware_version},
            {"device_order_number", id.order_number}, {"device_type", id.device_type},
            {"device_name", id.location_name}, {"device_keeps_flagged_points", id.is_s01_variant},
            {"remission", lms4xxx::RemissionName(config.scan.remission)}, {"config_yaml", yaml},
            {"ntp_enabled", config.ntp.enabled}, {"max_time_step_ms", config.ntp.max_time_step_ms},
            {"start_angle_deg", lms4xxx::ScanFixed::kStartAngleDeg},
            {"stop_angle_deg", lms4xxx::ScanFixed::kStopAngleDeg},
            {"angle_step_deg", lms4xxx::ScanFixed::kAngularResolutionDeg},
            {"output_rate", lms4xxx::ScanFixed::kOutputRate}, {"absolute_time_verified", false}
        };
        auto audit = nlohmann::ordered_json::object();
        const auto put = [&audit](const char *key, const auto &value) { if (value) audit[key] = *value; };
        const auto &a = driver.GetDeviceAudit();
        put("temperature_c", a.temperature_c);
        put("operating_hours_deci", a.operating_hours_deci);
        put("power_on_count", a.power_on_count);
        put("device_state", a.device_state);
        put("scan_frequency_centi_hz", a.scan_frequency_centi_hz);
        put("angular_resolution_1e4", a.angular_resolution_1e4);
        put("start_angle_1e4", a.start_angle_1e4);
        put("stop_angle_1e4", a.stop_angle_1e4);
        put("laser_trigger_source", a.laser_trigger_source);
        put("laser_timeout_s", a.laser_timeout_s);
        put("motor_sync_role", a.motor_sync_role);
        put("motor_sync_phase_deg", a.motor_sync_phase_deg);
        metadata["audit"] = std::move(audit);
        return metadata;
    }
} // namespace


Lms4xxxDriverApp::Lms4xxxDriverApp(const common::Config &run, const lms4xxx::AppConfig &config,
                                   const lms4xxx::LidarConfig &lidar) :
    instance_name_(lidar.id),
    driver_config_(config.DriverConfigFor(lidar)),
    output_(config.output),
    telemetry_interval_s_(config.telemetry_interval_s),
    stats_interval_s_(config.stats_interval_s),
    data_folder_path_(run.data_folder_path.string()),
    timestamp_(run.timestamp),
    config_path_((run.data_folder_path / fmt::format("config/config-lms4xxx_{}.yaml", run.timestamp)).string()) {
}


Lms4xxxDriverApp::~Lms4xxxDriverApp() {
    if (driver_) {
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
    driver_ = std::make_unique<lms4xxx::Lms4xxxDriver>(driver_config_);

    driver_->SetErrorCallback([name = instance_name_](std::error_code err, const std::string &detail) {
        g_log.Error("[{}] Error: {}{}", name, err.message(), detail.empty() ? "" : " (" + detail + ")");
    });

    if (data_folder_path_.empty()) {
        g_log.Error("[{}] Cannot set up the raw recorder (empty data folder path)", instance_name_);
        return false;
    }

    if (!driver_config_.ntp.enabled) {
        // Host reception is diagnostic, never substituted for device scan time.
        g_log.Warn("[{}] ntp.enabled=false: scans carry NO absolute time (device uptime only, wraps every "
                   "71.6 min); absolute time association needs an independently established clock mapping",
                   instance_name_);
    }

    auto ec = driver_->Connect();
    if (ec) {
        g_log.Error("[{}] Connect failed: {}", instance_name_, ec.message());
        return false;
    }

    if (stopped()) {
        return false;
    }
    ec = driver_->Configure();
    if (ec) {
        MarkFailed();
        g_log.Error("[{}] Configure failed: {}", instance_name_, ec.message());
        driver_->Disconnect();
        return false;
    }

    if (stopped()) {
        return false;
    }
    lms4xxx::RawRecorder::Config writer_config;
    writer_config.output_path = fmt::format("{}/lms4xxx/scan_{}_{}.bin", data_folder_path_, instance_name_, timestamp_);
    writer_config.instance = instance_name_;
    writer_config.session_timestamp = timestamp_;
    writer_config.metadata = DeviceMetadata(*driver_, driver_config_, config_path_);
    writer_config.queue_capacity = output_.queue_max_frames;
    writer_config.max_file_bytes = output_.max_file_bytes;
    writer_config.flush_interval_ms = output_.flush_interval_ms;
    recorder_ = std::make_unique<lms4xxx::RawRecorder>(std::move(writer_config));
    driver_->SetTelegramCallback([this](lms4xxx::RawRecord &&record) {
        recorder_->OnFrame(std::move(record));
    });

    g_log.Info(fmt::runtime(common::Markers::kLmsInitializedInstTpl), instance_name_);
    return true;
}


void Lms4xxxDriverApp::Run() {
    if (!driver_ || !recorder_) {
        RequestFailure();
        g_log.Error("Run() called without Init()");
        return;
    }

    // Before scanning so no frames are missed
    if (recorder_ && !recorder_->Start()) {
        RequestFailure();
        g_log.Error("[{}] Cannot start the raw recorder", instance_name_);
        return;
    }

    auto ec = driver_->StartScanning();
    if (ec) {
        if (recorder_) {
            recorder_->Stop();
        }
        RequestFailure();
        g_log.Error("[{}] Start scanning failed: {}", instance_name_, ec.message());
        return;
    }

    g_log.Info("[{}] Scanning started", instance_name_);

    using clock = std::chrono::steady_clock;
    const double stats_interval = stats_interval_s_;
    scan_start_ = clock::now();
    auto last_stats = scan_start_;
    auto last_telemetry_request = scan_start_;
    std::uint64_t rate_prev_frames = 0;
    std::uint64_t rate_prev_written = 0;
    std::uint64_t rate_prev_queued = 0;
    auto rate_prev_time = scan_start_;

    // Arms Main's no-data watchdog: from here on, silence is not expected.
    // A device that keeps the TCP connection open but stops streaming produces
    // no read error, no fault and no frames, so nothing else would end the run.
    scan_start_us_.store(common::TimeUtil::SteadyNowUs(), std::memory_order_release);

    // One telemetry round immediately, so even a short run carries a sample.
    if (telemetry_interval_s_ > 0.0) {
        driver_->RequestTelemetry();
    }

    while (!terminate_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kPollInterval);

        if (!driver_->IsScanning()) {
            g_log.Warn("[{}] Scanning stopped unexpectedly", instance_name_);
            RequestFailure();
            break;
        }
        // Request shutdown; the receiver still archives complete telegrams until stopped.
        if (driver_->HasFault()) {
            MarkFailed();
            g_log.Error("[{}] Fatal driver fault — terminating the Run (see the error above for the cause)",
                        instance_name_);
            terminate_.store(true, std::memory_order_release);
            break;
        }
        // Recording is mandatory: a dead writer ends the run like a failed Start()
        if (recorder_ && recorder_->HasFailed()) {
            MarkFailed();
            g_log.Error("[{}] Raw recorder stopped on a recording failure — terminating the Run (see the "
                        "[RawRecorder] error above for the cause)", instance_name_);
            terminate_.store(true, std::memory_order_release);
            break;
        }
        // Communication/recording failures stop the run. Scan-content checks run
        // only on the first scan; subsequent observations concern device time.
        {
            const auto drv = driver_->GetStatistics();
            const lms4xxx::RawRecorder::Statistics wr =
                    recorder_ ? recorder_->GetStatistics() : lms4xxx::RawRecorder::Statistics{};
            if (const char *what = lms4xxx::FirstDataLoss(drv, wr); what != nullptr) {
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

        // Replies are already archived as telegrams; retain a display-only snapshot.
        if (telemetry_interval_s_ > 0.0) {
            if (std::chrono::duration<double>(now - last_telemetry_request).count() >=
                telemetry_interval_s_) {
                last_telemetry_request = now;
                driver_->RequestTelemetry();
            }
            lms4xxx::TelemetrySample sample;
            if (driver_->TakeTelemetry(sample) && recorder_) {
                last_telemetry_ = sample;
            }
        }

        if (stats_interval > 0 && std::chrono::duration<double>(now - last_stats).count() >= stats_interval) {
            last_stats = now;
            const auto drv = driver_->GetStatistics();
            const lms4xxx::RawRecorder::Statistics wr =
                    recorder_ ? recorder_->GetStatistics() : lms4xxx::RawRecorder::Statistics{};
            // Only scans with readable time metadata contribute to scan rate; CoLa B also carries control replies.
            const double dt = std::chrono::duration<double>(now - rate_prev_time).count();
            const double scan_hz = dt > 0.0
                                       ? static_cast<double>(drv.frames_parsed - rate_prev_frames) / dt
                                       : 0.0;
            // app/services/driver_stats.py defines fps= as scans ACTUALLY
            // WRITTEN TO DISK per second, and that is what the dashboard card
            // shows for every other driver. Queue acceptance is a different
            // number and gets its own field.
            const double write_fps = dt > 0.0
                                         ? static_cast<double>(wr.scans_written - rate_prev_written) / dt
                                         : 0.0;
            const double queued_fps = dt > 0.0
                                          ? static_cast<double>(wr.scans_queued - rate_prev_queued) / dt
                                          : 0.0;
            rate_prev_frames = drv.frames_parsed;
            rate_prev_written = wr.scans_written;
            rate_prev_queued = wr.scans_queued;
            rate_prev_time = now;
            const auto uptime_s = static_cast<std::uint64_t>(std::chrono::duration<double>(now - scan_start_).
                count());
            // Field order and the double-space separator are a GUI contract
            // (app/services/driver_stats.py routes on the "[Statistics] [<id>] "
            // prefix and reads fps=; tools/check_contracts.py pins a rendered
            // sample). New fields are APPENDED, never interleaved.
            g_log.Info("[Statistics] [{}] up={}  rate={:.1f} Hz  fps={:.1f}  ntp={}  frames={}  parsed={}  "
                       "drop_ring={}  gaps={}  crc={}  frame_err={}  parse_err={}  written={}  drop_q={}  files={}  "
                       "bytes={}  queued={:.1f}  temp={}  unexpected={}  prelock={}  tstep_max_us={}  "
                       "utc_back={}  utc_repeat={}  ntp_device_loss={}  ntp_server_reachable={}  time_anomaly_events={}",
                       instance_name_, common::TimeUtil::HumanDuration(uptime_s), scan_hz, write_fps,
                       NtpStatusText(drv.ntp_status),
                       drv.frames_received, drv.frames_parsed, drv.frames_dropped, drv.counter_gaps, drv.crc_errors,
                       drv.framing_errors, drv.parse_errors, wr.scans_written, wr.frames_dropped, wr.files_created,
                       common::HumanBytes(wr.bytes_written), queued_fps,
                       last_telemetry_.temperature_c
                           ? fmt::format("{:.1f}", *last_telemetry_.temperature_c)
                           : "n/a",
                       drv.unexpected_replies, drv.prelock_scans_discarded, drv.max_time_step_us,
                       drv.utc_backwards, drv.utc_repeated, drv.device_no_ntp_events, drv.ntp_server_reachable,
                       drv.time_anomaly_events);
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
    if (const auto since_frame_us = driver_->MicrosSinceLastFrame(); since_frame_us > 0) {
        return since_frame_us;
    }
    // Nothing has arrived yet, so the subscription itself is the reference.
    const auto now_us = common::TimeUtil::SteadyNowUs();
    return now_us > started_us ? now_us - started_us : 0;
}


void Lms4xxxDriverApp::Shutdown() {
    scan_start_us_.store(0, std::memory_order_release);
    if (!driver_) {
        return;
    }

    // Device measurement begins during Configure(), before IsScanning() is true.
    // Cancelled initialization and writer startup failures need the same rollback.
    if (const auto ec = driver_->StopScanning()) {
        MarkFailed();
        g_log.Error("[{}] StopScanning failed: {}", instance_name_, ec.message());
    }

    // Stop the writer first so the summary reports the on-disk totals
    if (recorder_) {
        recorder_->Stop();
    }

    const auto cleanup_ec = driver_->Disconnect(); // idempotent; preserves the stop-measurement result
    if (cleanup_ec) MarkFailed();
    const auto shutdown = driver_->GetMeasurementShutdownStatus();
    const auto drv = driver_->GetStatistics();
    const lms4xxx::RawRecorder::Statistics wr =
            recorder_ ? recorder_->GetStatistics() : lms4xxx::RawRecorder::Statistics{};
    auto result = lms4xxx::AssessRecording(drv, wr, driver_->HasFault(),
                                         recorder_ && recorder_->HasFailed(), HasFailed());
    if (cleanup_ec) result.failure_reasons.push_back("device stop measurement not confirmed: " + cleanup_ec.message());
    if (result.Failed()) MarkFailed();
    final_statistics_ = {
        {"instance", instance_name_}, {"format", "lms4xxx-cola-b"}, {"format_version", 1},
        {"frames_received", drv.frames_received},
        {"frames_parsed", drv.frames_parsed}, {"dropped_ring", drv.frames_dropped},
        {"counter_gaps", drv.counter_gaps}, {"crc_errors", drv.crc_errors},
        {"framing_errors", drv.framing_errors}, {"parse_errors", drv.parse_errors},
        {"frames_queued", wr.frames_queued}, {"frames_written", wr.frames_written},
        {"scans_queued", wr.scans_queued}, {"scans_written", wr.scans_written},
        {"dropped_writer", wr.frames_dropped}, {"bytes_written", wr.bytes_written},
        {"files_created", wr.files_created}, {"ntp_status", NtpStatusText(drv.ntp_status)},
        {"prelock_scans_discarded", drv.prelock_scans_discarded}, {"max_time_step_us", drv.max_time_step_us},
        {"utc_backwards", drv.utc_backwards}, {"utc_repeated", drv.utc_repeated},
        {"clock_step_events", drv.clock_step_events}, {"device_no_ntp_events", drv.device_no_ntp_events},
        {"time_anomaly_events", drv.time_anomaly_events},
        {"ntp_server_reachable", drv.ntp_server_reachable}, {"absolute_time_verified", false},
        {"non_scan_frames", drv.non_scan_frames}, {"scan_candidates", drv.ScanCandidateFrames()},
        {"shutdown_complete", false}, {"data_integrity_failed", result.data_integrity_failed},
        {"measurement_start_requested", shutdown.measurement_start_requested},
        {"measurement_cleanup_attempted", shutdown.cleanup_attempted},
        {"stop_measurement_command_attempted", shutdown.stop_measurement_command_attempted},
        {"stop_measurement_confirmed", shutdown.stop_measurement_confirmed},
        {"measurement_cleanup_error", cleanup_ec ? cleanup_ec.message() : ""},
        {"time_quality_degraded", result.time_quality_degraded}, {"failure_reasons", result.failure_reasons},
        {"recording_incomplete", HasFailed()}
    };
    const auto duration_s = scan_start_ == std::chrono::steady_clock::time_point{}
                                ? 0ull
                                : static_cast<std::uint64_t>(std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - scan_start_)
                                    .count());
    g_log.Info("[Statistics] [{}] Final: duration={}  frames={}  parsed={}  delivery={:.1f}%  ntp={}  "
               "dropped_ring={}  counter_gaps={}  crc_errors={}  framing_errors={}  parse_errors={}  "
               "unexpected_replies={}  scans_written={}  dropped_queue={}  bytes={}  files={}  prelock={}  "
               "tstep_max_us={}  utc_back={}  utc_repeat={}  clock_step_events={}  ntp_device_loss={}  "
               "ntp_server_reachable={}  control_frames={}  scan_candidates={}  time_anomaly_events={}",
               instance_name_, common::TimeUtil::HumanDuration(duration_s), drv.frames_received, drv.frames_parsed,
               drv.DeliveryRate(), NtpStatusText(drv.ntp_status), drv.frames_dropped, drv.counter_gaps,
               drv.crc_errors, drv.framing_errors, drv.parse_errors, drv.unexpected_replies,
               wr.scans_written, wr.frames_dropped, common::HumanBytes(wr.bytes_written), wr.files_created,
               drv.prelock_scans_discarded, drv.max_time_step_us, drv.utc_backwards, drv.utc_repeated,
               drv.clock_step_events, drv.device_no_ntp_events, drv.ntp_server_reachable,
               drv.non_scan_frames, drv.ScanCandidateFrames(), drv.time_anomaly_events);

    driver_.reset(); // Makes the destructor and repeated Shutdown() no-ops.
    final_statistics_["shutdown_complete"] = true;

    if (HasFailed()) {
        g_log.Error("[{}] LMS4xxx shutdown complete; recording is INCOMPLETE: {}", instance_name_, result.Summary());
    } else {
        g_log.Info(fmt::runtime(common::Markers::kLmsShutdownInstTpl), instance_name_);
    }
}
