#include "capture_runner.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include "logger.h"
#include "sensor_sync_hub.h"
#include "time_util.h"
#include "stats.h"
#include "util.h"
#include "ebus/camera_session.h"

namespace gox {
    namespace {
        common::DriverLog g_log{"GoX"};


        // One tick for every read that has to reach the camera over GVCP: the
        // PTP health guard and the device telemetry row.
        constexpr double kDevicePollIntervalS = 5.0;

        // SensorSync guards (same numbers as the fx10 driver): no timing-log growth
        // for this long = the Teensy USB link is down; armed this long without the
        // shared session starting = another registered camera never armed.
        constexpr double kSensorSyncStallAbortS = 15.0;
        constexpr double kSensorSyncStartWaitS = 30.0;
    } // namespace


    CaptureRunner::CaptureRunner(AppConfig cfg, StopController *stop, std::shared_ptr<common::SensorSyncHub> sync)
        : cfg_(std::move(cfg)), stop_(stop), sync_(cfg_.sensor_trigger.enabled ? std::move(sync) : nullptr) {
    }


    CaptureRunner::~CaptureRunner() {
        if (initialized_ && !shutdown_done_ && !stop_->StopRequested()) {
            // Reached only via stack unwinding: an exception escaped between
            // Init() and Shutdown() (e.g. out of the poll loop). Record the
            // stop as an error so the reported outcome agrees with the fatal path.
            stop_->RequestStop(StopReason::kError);
        }
        try {
            Shutdown();
        } catch (...) {
            // Never throw out of a destructor (may run during unwinding).
        }
    }

    bool CaptureRunner::Init(const std::string &session_dir) {
        GenUuidV4(session_uuid_);
        start_rt_ = common::TimeUtil::RealtimeNowNs();
        session_dir_ = session_dir;
        std::error_code ec;
        std::filesystem::create_directories(session_dir_, ec);
        if (ec) {
            g_log.Error("Cannot create output directory: {}", ec.message());
            return false;
        }

        const std::string timing_log = sync_ ? sync_->LogPath().string() : std::string();
        for (const CameraConfig &cam: cfg_.cameras) {
            if (cam.enabled) {
                sessions_.push_back(std::make_unique<ebus::CameraSession>(cam, cfg_, session_uuid_, stop_, timing_log));
            }
        }
        for (auto &session: sessions_) {
            try {
                session->Start(session_dir_);
                // The camera is armed (TriggerMode On, AcquisitionStart): tell the shared
                // SensorSync session. The pulses start once every registered camera has
                // armed — normally the FX10, which arms in its Run — or right here when
                // this driver is the only participant. Never before AcquisitionStart:
                // a pulse the camera is not ready for is a frame that never exists.
                if (sync_) {
                    if (sync_->Arm(SensorSyncOwner(session->id()))) {
                        g_log.Info("[{}] SensorSync session running; timing log {}", session->id(),
                                   sync_->LogPath().string());
                    } else {
                        g_log.Info("[{}] armed; SensorSync pulses start once every registered camera has armed",
                                   session->id());
                    }
                }
            } catch (const std::exception &e) {
                // A stop that landed mid-bring-up (Ctrl+C during a PTP wait, or
                // a running camera's writer failing) surfaces as an exception
                // out of whatever step was active — report the concrete cause.
                const bool interrupted = stop_->StopRequested() &&
                                         (stop_->Reason() == StopReason::kSignal || stop_->Reason() ==
                                          StopReason::kExternal);
                last_error_ = interrupted ? std::string("interrupted during bring-up: ") + e.what() : e.what();
                g_log.Error("Startup failed: {}", last_error_);
                if (!interrupted) {
                    stop_->RequestStop(StopReason::kError);
                }
                for (auto &s: sessions_) {
                    s->StopAndJoin();
                }
                return false;
            }
            if (stop_->StopRequested()) {
                // e.g. Ctrl+C during a PTP wait. The remaining cameras were never
                // brought up, so this is not a successful start: reporting it as
                // one would emit the "initialized" markers for cameras that are
                // not running and leave the GUI showing a healthy driver.
                last_error_ = "interrupted during bring-up (" + std::string(StopReasonName(stop_->Reason())) + ")";
                g_log.Warn("Startup interrupted: {}", last_error_);
                for (auto &s: sessions_) {
                    s->StopAndJoin();
                }
                return false;
            }
        }

        initialized_ = true;
        return true;
    }


    void CaptureRunner::MonitorLoop(const std::function<bool()> &external_stop) {
        if (!initialized_) {
            return;
        }

        // max_duration_s measures CAPTURE time
        capture_start_mono_ = common::TimeUtil::MonotonicNowNs();
        const double stats_interval_s = cfg_.stats_interval_s;
        uint64_t last_stats_mono = capture_start_mono_;
        uint64_t last_device_poll_mono = capture_start_mono_;
        while (!stop_->StopRequested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (external_stop && external_stop()) {
                stop_->RequestStop(StopReason::kExternal);
                break;
            }
            const uint64_t now_mono = common::TimeUtil::MonotonicNowNs();
            const uint64_t uptime_s = (now_mono - capture_start_mono_) / 1000000000ull;

            if (cfg_.output.max_duration_s > 0 &&
                static_cast<double>(now_mono - capture_start_mono_) >= cfg_.output.max_duration_s * 1e9) {
                g_log.Info("max_duration_s ({}) reached", cfg_.output.max_duration_s);
                stop_->RequestStop(StopReason::kLimitReached);
                break;
            }
            if (stats_interval_s > 0 && static_cast<double>(now_mono - last_stats_mono) >= stats_interval_s * 1e9) {
                const double actual = static_cast<double>(now_mono - last_stats_mono) / 1e9;
                last_stats_mono = now_mono;
                for (auto &s: sessions_) {
                    s->PollStreamStats();
                    g_log.Info("{}", s->Reporter().PeriodicLine(actual, uptime_s));
                }
            }
            // Fail-fast: the first dropped, lost or incomplete frame ends the whole
            // rig now rather than marking the session failed at the end - an
            // incomplete recording is worthless and the operator restarts at once.
            // Stream-layer counters live in the SDK, so refresh them first.
            bool loss = false;
            for (auto &s: sessions_) {
                s->PollStreamStats();
                if (const std::string what = s->FirstLossDescription(); !what.empty()) {
                    last_error_ = "[" + s->id() + "] first data-loss event: " + what;
                    g_log.Error("{} - stopping the rig (fail-fast: the recording would be incomplete)", last_error_);
                    stop_->RequestStop(StopReason::kError);
                    loss = true;
                    break;
                }
            }
            if (loss) {
                break;
            }
            // SensorSync (the frames' only path to GPS time): the board's integrity
            // verdict, a stalled timing log and a session that never started are
            // all "frames without timing", which are worthless — stop the rig.
            if (sync_) {
                std::string why;
                if (!sync_->Ok()) {
                    why = "SensorSync trigger log/protocol integrity failed (I/O, rejected command, restart or "
                          "event loss): " + sync_->LastError();
                } else if (const double stalled = sync_->StalledSeconds(); stalled > kSensorSyncStallAbortS) {
                    why = fmt::format("SensorSync timing log has not grown for {:.0f} s (Teensy USB link lost?)",
                                      stalled);
                } else {
                    for (auto &s: sessions_) {
                        const double waiting = sync_->WaitingSeconds(SensorSyncOwner(s->id()));
                        if (waiting > kSensorSyncStartWaitS) {
                            why = fmt::format("SensorSync session has not started {:.0f} s after [{}] armed "
                                              "(another registered camera never armed?)", waiting, s->id());
                            break;
                        }
                    }
                }
                if (!why.empty()) {
                    last_error_ = why;
                    g_log.Error("{} - stopping the rig (frames without timing observations are worthless)", why);
                    stop_->RequestStop(StopReason::kError);
                    break;
                }
            }
            // Device poll. Two jobs on one tick, guard first so the telemetry
            // row carries the status the guard just read:
            //  1. PTP guard - the camera must stay in "slave" with a usable
            //     clock accuracy, otherwise the device timestamps stop being
            //     traceable to the grandmaster and the recording is no longer
            //     what it claims.
            //  2. telemetry.jsonl - temperatures, Counter0, PAUSE frames.
            // The ptp.enabled gate sits on the guard call, not on the tick:
            // ptp.enabled=false is a warned configuration (no absolute time), and
            // telemetry must still run under it.
            if (static_cast<double>(now_mono - last_device_poll_mono) >= kDevicePollIntervalS * 1e9) {
                last_device_poll_mono = now_mono;
                for (auto &s: sessions_) {
                    if (cfg_.ptp.enabled && !s->CheckPtpHealth()) {
                        last_error_ = "[" + s->id() + "] PTP synchronization lost during capture";
                        stop_->RequestStop(StopReason::kError);
                        break;
                    }
                    s->PollDeviceTelemetry();
                }
            }
        }
    }


    std::optional<std::uint64_t> CaptureRunner::MicrosSinceLastData() const {
        std::optional<std::uint64_t> worst;
        for (const auto &s: sessions_) {
            const auto silent_us = s->MicrosSinceLastData();
            if (silent_us && (!worst || *silent_us > *worst)) {
                worst = silent_us;
            }
        }
        return worst;
    }

    bool CaptureRunner::Shutdown() {
        if (shutdown_done_) {
            return clean_;
        }
        shutdown_done_ = true;
        // Pulses stop before the cameras do: a camera stopped under a running pulse
        // train would only collect missed triggers. The first disarm ends the rig's
        // shared session (the FX10's teardown may already have); later ones are no-ops
        if (sync_) {
            sync_->Disarm(sessions_.empty() ? std::string("gox") : SensorSyncOwner(sessions_.front()->id()));
        }
        if (!initialized_) {
            // Init() already tore down, logged and kept the concrete error.
            clean_ = false;
            return clean_;
        }

        g_log.Info("stopping (reason: {})", StopReasonName(stop_->Reason()));
        for (auto &s: sessions_) {
            s->StopAndJoin();
        }
        const uint64_t uptime_s = capture_start_mono_ != 0
                                      ? (common::TimeUtil::MonotonicNowNs() - capture_start_mono_) / 1000000000ull
                                      : 0;
        bool all_clean = true;
        for (auto &s: sessions_) {
            g_log.Info("{}", s->Reporter().FinalSummary(uptime_s));
            all_clean = all_clean && s->Clean();
        }

        if (stop_->Reason() == StopReason::kError) {
            if (last_error_.empty()) {
                last_error_ = "session stopped on an error (see the log above for the concrete cause)";
            }
            clean_ = false;
        } else if (!all_clean) {
            last_error_ = "frame drops / incomplete frames / stream errors detected (see the final statistics above)";
            clean_ = false;
        } else if (sync_ && !sync_->Ok()) {
            // The frames are complete but their timing observations are not
            last_error_ = "SensorSync timing session failed integrity checks (" + sync_->LastError() + ")";
            clean_ = false;
        }
        return clean_;
    }
} // namespace gox
