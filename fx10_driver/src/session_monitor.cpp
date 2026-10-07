#include "session_internal.h"

#include <chrono>
#include <thread>
#include "stats_line.h"
#include "time_util.h"

namespace fx10 {
using namespace session_detail;

bool Session::Impl::Connection::RecordTelemetry(const std::string &phase, bool establish_baseline) {
    try {
        fx10::DeviceTelemetry sample;
        sample.host_realtime_ns = common::TimeUtil::RealtimeNowNs();
        sample.host_monotonic_ns = common::TimeUtil::MonotonicNowNs();
        if (control && receiver.Device() && receiver.Device()->IsConnected() &&
            receiver.GetFatalKind() != fx10::StreamReceiver::FatalKind::kLinkLost) {
            sample.missed_raw = control->TryGetInt(fx10::node::kMissedTriggerCount);
            sample.temp_pcb_c = control->TryReadTemperature("ProcPCB");
            sample.temp_fpga_c = control->TryReadTemperature("FPGA");
        }
        if (establish_baseline) missed_baseline = sample.missed_raw.value_or(-1);
        if (sample.missed_raw) {
            if (*sample.missed_raw < 0 || (previous_missed && *sample.missed_raw < *previous_missed))
                counter_regressed = true;
            previous_missed = sample.missed_raw;
        }
        if (missed_baseline >= 0) sample.missed_baseline = missed_baseline;
        sample.counter_regressed = counter_regressed;
        sample.counter_binding_verified = counter_binding_verified;
        sample.read_finished_monotonic_ns = common::TimeUtil::MonotonicNowNs();
        latest_telemetry = sample;
        auto row = fx10::BuildTelemetryJson(sample, phase);
        row["acquisition_started"] = acquisition_started;
        row["frames_delivered"] = receiver.FramesDelivered();
        row["frames_written"] = recorder ? recorder->FramesWrittenTotal() : 0;
        row["frame_counts_semantics"] = "frames_delivered = admitted to recording queue; frames_written = recorder count; separate observations, not a simultaneous snapshot";
        telemetry.Append(row);
        return true;
    } catch (const std::exception &e) {
        metadata_failed = true;
        g_log.Error("Device telemetry recording failed: {}", e.what());
        return false;
    }
}

void Session::MonitorLoop() {
    using clock = std::chrono::steady_clock;
    auto &cfg = impl_->config;
    auto &s = *impl_->session;

    const auto t_start = clock::now();
    const double stats_interval = cfg.logging.stats_interval_s;
    const auto after = [](double seconds) {
        return std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(seconds));
    };
    // Two INDEPENDENT cadences. The thermal guard is a safety check and must
    // not be switchable off by logging.stats_interval_s. (The disk floor is
    // rig-wide now and lives in Main; see Guards: in config-main.yaml.)
    auto next_stats = t_start + after(stats_interval > 0 ? stats_interval : 3600.0);
    auto next_device = t_start + after(kDeviceTelemetryIntervalS);
    // Measured line-rate basis: frames delivered / written since the previous stats line
    std::uint64_t rate_prev_frames = 0;
    std::uint64_t rate_prev_written = 0;
    auto rate_prev_time = t_start;
    // Latest device telemetry, refreshed on its own cadence and merely printed
    // by the statistics line.
    std::optional<double> temp_pcb = s.latest_telemetry.temp_pcb_c;
    std::optional<double> temp_fpga = s.latest_telemetry.temp_fpga_c;
    bool over_temperature = false;

    // A freely controlled external trigger may intentionally pause. SensorSync
    // is different: this process commanded periodic pulses, so silence is a fault.
    const bool watchdog_applies = cfg.acquisition.trigger.mode == fx10::TriggerMode::kFreerun ||
                                  cfg.sensor_trigger.enabled; // this process commanded periodic pulses

    while (true) {
        // Republish the receiver's liveness for Main. impl_->session is owned by
        // THIS thread (teardown resets it), so Main must never reach into it; it
        // reads this atomic instead.
        data_reference_us_.store(
            watchdog_applies ? s.receiver.LastDataReferenceUs().value_or(0) : 0, std::memory_order_release);

        if (StopRequested()) {
            impl_->stop_reason = "external-stop";
            break;
        }
        if (s.receiver.Failed() || (s.recorder && s.recorder->Failed())) {
            break; // classified in teardownSession_
        }
        // Fail-fast: the first lost or unrecorded frame ends the whole rig now
        // rather than marking the run DEGRADED at the end — an incomplete
        // recording is worthless and the operator restarts immediately.
        if (s.receiver.LossSeen() || (s.recorder && s.recorder->LossSeen())) {
            g_log.Error("First data-loss event (see the warning above) — stopping the rig (fail-fast: the "
                        "recording would be incomplete)");
            impl_->stop_reason = "data-loss";
            impl_->last_exit_code = 10;
            break;
        }
        if (impl_->sync_participant && !impl_->sync->Ok()) {
            g_log.Error("Sensor trigger log/protocol integrity failed (I/O, rejected command, restart or event "
                        "loss): {}; stopping", impl_->sync->LastError());
            impl_->stop_reason = "trigger-log-failure";
            impl_->last_exit_code = 20;
            break;
        }
        if (impl_->sync_participant && impl_->sync->StalledSeconds() > kTriggerLogStallAbortS) {
            // Even without an explicit I/O/protocol error, sustained silence
            // means timing observations are unavailable.
            g_log.Error("Sensor trigger timing log has not grown for {:.0f} s (Teensy USB link lost?) — "
                        "frames without timing are worthless, stopping",
                        impl_->sync->StalledSeconds());
            impl_->stop_reason = "trigger-log-stalled";
            impl_->last_exit_code = 20;
            break;
        }
        if (impl_->sync_participant && impl_->sync->WaitingSeconds(kSensorSyncOwner) > kSensorSyncStartWaitS) {
            // Every frame recorded before the pulses run would have no time source
            g_log.Error("SensorSync session has not started {:.0f} s after this camera armed (another registered "
                        "camera never armed?); stopping", impl_->sync->WaitingSeconds(kSensorSyncOwner));
            impl_->stop_reason = "trigger-session-not-started";
            impl_->last_exit_code = 20;
            break;
        }
        const double elapsed = std::chrono::duration<double>(clock::now() - t_start).count();
        if (cfg.recording.max_duration_s > 0.0 && elapsed >= cfg.recording.max_duration_s) {
            impl_->stop_reason = "duration-reached";
            break;
        }
        if (cfg.recording.max_frames > 0 && s.receiver.FramesDelivered() >= cfg.recording.max_frames) {
            impl_->stop_reason = "frame-count-reached";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // --- device telemetry (blocking control-channel reads) ---------------
        if (clock::now() >= next_device) {
            next_device = clock::now() + after(kDeviceTelemetryIntervalS);
            // Thread-safe sources only: atomics + GenICam control-channel reads
            // (acquisition/recording workers own separate Counters fields;
            // the owner reads the whole ledger only after both workers join)
            if (!s.RecordTelemetry("periodic")) {
                impl_->stop_reason = "metadata-failure";
                impl_->last_exit_code = 20;
                break;
            }
            temp_pcb = s.latest_telemetry.temp_pcb_c;
            temp_fpga = s.latest_telemetry.temp_fpga_c;
            const auto missed_delta = fx10::MissedTriggerDelta(s.latest_telemetry);
            if (s.counter_binding_verified && s.counter_regressed) {
                g_log.Error("Missed-trigger counter regressed; reset/wrap invalidates the session delta");
                impl_->stop_reason = "counter-discontinuity";
                impl_->last_exit_code = 10;
                break;
            }

            // A missed trigger is a frame that does not exist: fail-fast here too
            // (the camera counter is only readable on this slow cadence).
            if (missed_delta && *missed_delta > 0) {
                g_log.Error("Camera reports {} missed trigger(s) since acquisition start — stopping the rig "
                            "(fail-fast: frames are missing from the recording)", *missed_delta);
                impl_->stop_reason = "missed-triggers";
                impl_->last_exit_code = 10;
                break;
            }

            // Manual Table 8 (p.44): the camera cancels operation at 80 C
            // (processing board) / 90 C (FPGA); Troubleshooting (p.43) lists
            // "camera shuts down unexpectedly" under too-high temperature. Warn
            // before that, but never stop the recording on our own.
            const bool hot = (temp_pcb && *temp_pcb >= kProcPcbLimitC - kThermalWarnMarginC) ||
                             (temp_fpga && *temp_fpga >= kFpgaLimitC - kThermalWarnMarginC);
            const bool cooled = (!temp_pcb || *temp_pcb <= kProcPcbLimitC - kThermalRearmMarginC) &&
                                (!temp_fpga || *temp_fpga <= kFpgaLimitC - kThermalRearmMarginC);
            if (!over_temperature && hot) {
                over_temperature = true;
                const auto celsius = [](const std::optional<double> &v) {
                    return v ? fmt::format("{:.1f}", *v) : std::string("n/a");
                };
                g_log.Warn("Camera is approaching its thermal limit (processing board {} °C of {:.0f}, "
                           "FPGA {} °C of {:.0f}, manual p.44) — it cancels operation above those; "
                           "improve cooling or the recording will end with the camera shutting down",
                           celsius(temp_pcb), kProcPcbLimitC, celsius(temp_fpga), kFpgaLimitC);
            } else if (over_temperature && cooled) {
                over_temperature = false;
            }
        }

        if (stats_interval > 0 && clock::now() >= next_stats) {
            next_stats = clock::now() + after(stats_interval);
            // Missed triggers SINCE this session started: Counter1 is reset at
            // bring-up, but a camera that refused the Reset (or the counter read
            // itself) leaves the field unknown rather than zero.
            const auto missed_delta = fx10::MissedTriggerDelta(s.latest_telemetry);
            // Sensor's actual line rate over the last interval — under external
            // trigger this should track acquisition.frame_rate_hz (the commanded
            // SensorSync pulse rate); a lower value means missed triggers or RX loss
            const std::uint64_t frames_now = s.receiver.FramesDelivered();
            // Frames that actually reached the .bil on disk (excludes geometry
            // drops and padding); fps < rate means the recorder is losing frames
            // the transport delivered. Same window as rate.
            const std::uint64_t written_now = s.recorder ? s.recorder->FramesWrittenTotal() : 0;
            const auto rate_now = clock::now();
            const double rate_dt = std::chrono::duration<double>(rate_now - rate_prev_time).count();
            const double rate_hz = rate_dt > 0.0
                                       ? static_cast<double>(frames_now - rate_prev_frames) / rate_dt
                                       : 0.0;
            const double write_fps = rate_dt > 0.0
                                         ? static_cast<double>(written_now - rate_prev_written) / rate_dt
                                         : 0.0;
            rate_prev_frames = frames_now;
            rate_prev_written = written_now;
            rate_prev_time = rate_now;
            // The rendering (and its GUI contract) lives in stats_line.cpp.
            fx10::StatsSample sample;
            sample.frames = frames_now;
            sample.rate_hz = rate_hz;
            sample.write_fps = write_fps;
            sample.missed_triggers = missed_delta;
            sample.temp_pcb_c = temp_pcb;
            sample.temp_fpga_c = temp_fpga;
            g_log.Info("{}", fx10::FormatStatsLine(sample));
        }
    }
}


} // namespace fx10
