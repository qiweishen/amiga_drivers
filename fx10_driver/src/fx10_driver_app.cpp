#include "fx10_driver_app.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <spdlog/spdlog.h>
#include <string_view>
#include <thread>

#include "app_config.h"
#include "envi_recorder.h"
#include "pixel_format.h"
#include "sensor_trigger_log.h"
#include "stats_line.h"
#include "logger.h"
#include "wavelengths.h"
#include "driver_markers.h"
#include "ebus/camera_control.h"
#include "ebus/env_bootstrap.h"
#include "ebus/stream_receiver.h"
#include "time_util.h"
#include "utility.h"


namespace {
    // App-level module token: this file's error lines drive the GUI health
    // machine. All other fx10 files log under the internal "FX10" module.
    common::DriverLog g_log{std::string(common::Markers::kModuleFx10)};

    // No timing-log growth for this long = the Teensy USB link is down (the
    // board emits a #H health line at least every 5 s while a session runs).
    constexpr double kTriggerLogStallAbortS = 15.0;

    // Device telemetry is a blocking GVCP round trip that shares the link with
    // the stream, so it gets a slow cadence of its own and is NEVER tied to
    // logging.stats_interval_s (which an operator may set to 0).
    constexpr double kDeviceTelemetryIntervalS = 60.0;

    // Manual Table 8 (p.44): the camera CANCELS operation above these internal
    // temperatures. Warn before that happens; re-arm after a 5 C drop so a
    // camera sitting near the line does not flood the log.
    constexpr double kProcPcbLimitC = 80.0;
    constexpr double kFpgaLimitC = 90.0;
    constexpr double kThermalWarnMarginC = 5.0;
    constexpr double kThermalRearmMarginC = 10.0;
}


// One Session per camera connection
struct Fx10DriverApp::Impl {
    struct Session {
        fx10::Counters counters;
        std::unique_ptr<fx10::EnviRecorder> recorder; // built in startStreaming_
        fx10::StreamReceiver receiver;
        std::unique_ptr<fx10::CameraControl> control; // built after connect, before the stream open
        std::unique_ptr<fx10::SensorTriggerLog> trigger_log; // opened in bringUpSession_
        fx10::RecorderInit recorder_init;
        fx10::CameraControl::Geometry geometry;
        std::uint32_t bytes_per_pixel = 2;
        std::int64_t missed_baseline = -1;

        explicit Session(const fx10::NetworkConfig &network) : receiver(network, counters) {
        }
    };

    fx10::AppConfig config;
    std::unique_ptr<Session> session;

    std::string stop_reason = "completed"; // logged by EnviRecorder::Stop as the exit reason
    int last_exit_code = 0; // standalone exit-code semantics, reported at shutdown
    bool retryable_link_loss = false;
    bool run_incomplete = false; // sticky across connection sessions, checked only after Run joins
};


Fx10DriverApp::Fx10DriverApp(const common::Config &config) : impl_(std::make_unique<Impl>()) {
    // Actual config loading is deferred to Init()
    config_path_ = config.fx10_config_path;
    data_folder_path_ = config.data_folder_path;
}


Fx10DriverApp::~Fx10DriverApp() {
    Shutdown();
}


bool Fx10DriverApp::StopRequested() const {
    return terminate_.load(std::memory_order_acquire) || (external_stop_ && external_stop_());
}


void Fx10DriverApp::SleepInterruptible(int total_ms) const {
    while (total_ms > 0 && !StopRequested()) {
        const int slice = total_ms < 100 ? total_ms : 100;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        total_ms -= slice;
    }
}


bool Fx10DriverApp::Init(const std::function<bool()> &external_stop) {
    external_stop_ = external_stop;

    // Load + validate the driver YAML
    try {
        impl_->config = fx10::LoadAppConfig(config_path_);
    } catch (const fx10::ConfigError &e) {
        g_log.Error("FX10 config error: {}", e.what());
        return false;
    }

    impl_->config.recording.output_dir = data_folder_path_ / "fx10";
    std::error_code ec;
    std::filesystem::create_directories(impl_->config.recording.output_dir, ec);
    if (ec) {
        g_log.Error("FX10 cannot create output directory: {}", ec.message());
        return false;
    }

    // GenICam environment
    common::Ebus::BootstrapEnv();

    switch (BringUpSession()) {
        case BringUp::kStopped:
            g_log.Warn("FX10 bring-up interrupted by shutdown request");
            return false;
        case BringUp::kFailed:
            g_log.Error("FX10 startup failed (standalone exit code {})", impl_->last_exit_code);
            return false;
        case BringUp::kOk:
            break;
    }

    g_log.Info("{}", common::Markers::kFx10Initialized);
    init_ok_ = true;
    return true;
}


Fx10DriverApp::BringUp Fx10DriverApp::BringUpSession() const {
    auto &cfg = impl_->config;
    impl_->session = std::make_unique<Impl::Session>(cfg.network);
    auto &s = *impl_->session;

    const auto abandon = [this, &s](int exit_code) {
        impl_->last_exit_code = exit_code;
        s.receiver.Disconnect();
        impl_->session.reset();
        return BringUp::kFailed;
    };
    // The eBUS calls below block without a cancellation API
    const auto stopped = [this, &s]() {
        if (!StopRequested()) {
            return false;
        }
        s.receiver.Disconnect();
        impl_->session.reset();
        return true;
    };

    try {
        // Serial port first
        if (cfg.sensor_trigger.enabled) {
            if (cfg.acquisition.trigger.mode == fx10::TriggerMode::kFreerun) {
                g_log.Warn("sensor_trigger is enabled but acquisition.trigger.mode is freerun — the camera "
                    "ignores the trigger pulses; the timing log still records exposure strobes");
            }
            g_log.Info("SensorSync observations and camera BlockIDs have independent counters; "
                       "use the line index and a verified anchor, never infer association from padding");
            s.trigger_log = std::make_unique<fx10::SensorTriggerLog>(cfg.sensor_trigger.port);
            s.trigger_log->Open();
        }
        if (stopped()) {
            return BringUp::kStopped;
        }
        s.receiver.Connect(cfg.device);
        if (stopped()) {
            return BringUp::kStopped;
        }
        // Factory baseline before the stream open: the load resets GevSCPSPacketSize
        if (!fx10::PrepareFactoryDefaults(s.receiver, [this] { return StopRequested(); })) {
            s.receiver.Disconnect();
            impl_->session.reset();
            return BringUp::kStopped;
        }
        s.control = std::make_unique<fx10::CameraControl>(*s.receiver.Device());
        if (stopped()) {
            return BringUp::kStopped;
        }
        s.receiver.OpenStream();

        s.control->ApplyAcquisitionConfig(cfg.acquisition, cfg.features.raw);
        s.geometry = s.control->ReadGeometry();
        if (stopped()) {
            return BringUp::kStopped;
        }

        const int expected_bands = cfg.acquisition.ExpectedBands();
        if (s.geometry.height != expected_bands) {
            g_log.Warn(
                "Camera reports {} image rows, config implies {} spectral bands; calibrated coverage is unverified",
                s.geometry.height, expected_bands);
        }

        const auto try_get_string = [&s](const char *node) {
            try {
                return s.control->GetString(node);
            } catch (const fx10::ControlError &) {
                return std::string("?");
            }
        };

        // Storage layout after the receiver's unpack step (packed formats land
        // on disk as canonical uint16 — the ENVI contract never sees packing)
        const auto *pf = fx10::GetPixelFormatInfo(cfg.acquisition.pixel_format);
        s.bytes_per_pixel = pf != nullptr ? pf->storage_bpp : 2;
        s.recorder_init.samples = static_cast<std::uint32_t>(s.geometry.width);
        s.recorder_init.bands = static_cast<std::uint32_t>(s.geometry.height);
        s.recorder_init.bytes_per_pixel = s.bytes_per_pixel;
        s.recorder_init.pixel_format = s.geometry.pixel_format;
        s.recorder_init.expected_frame_rate_hz = cfg.acquisition.frame_rate_hz;
        s.recorder_init.data_type = s.bytes_per_pixel == 1 ? fx10::EnviDataType::kUint8 : fx10::EnviDataType::kUint16;
        s.recorder_init.wavelengths = fx10::ResolveForGeometry(
            cfg.recording.wavelengths, try_get_string("DeviceSerialNumber"), s.geometry.width, s.geometry.height,
            s.geometry.offset_x, s.geometry.offset_y, cfg.acquisition.status_line);
        // What the CAMERA ended up with, not what the config asked for: both
        // nodes have their own increments and bounds, so the requested value is
        // not necessarily the one every line in this file was taken with.
        const auto read_back = [&s](const char *node, double fallback, const char *unit) {
            const auto v = s.control->TryGetFloat(node);
            return fmt::format("{:.4g} {}{}", v.value_or(fallback), unit, v ? "" : " (requested; not read back)");
        };
        s.recorder_init.description =
                "vendor: " + try_get_string("DeviceVendorName") +
                "\nmodel: " + try_get_string("DeviceModelName") +
                "\nserial: " + try_get_string("DeviceSerialNumber") +
                "\npixel format: " + s.geometry.pixel_format +
                "\nspatial binning (final readback verified): " + std::to_string(cfg.acquisition.spatial_binning) +
                "\nspectral binning (final readback verified): " + std::to_string(cfg.acquisition.spectral_binning) +
                "\nimage offsets (read back; -1 unknown): " + std::to_string(s.geometry.offset_x) + "," +
                    std::to_string(s.geometry.offset_y) +
                // The FX10 exposure node is microseconds; the config is ms.
                "\nexposure: " + read_back(fx10::node::kExposureTime, cfg.acquisition.exposure_ms * 1000.0, "us") +
                // Under an external trigger the camera's own rate node is
                // disabled, so its value would be a stale fiction: report the
                // commanded pulse rate instead.
                "\nframe rate: " + (cfg.acquisition.trigger.mode == fx10::TriggerMode::kFreerun
                                        ? read_back(fx10::node::kFrameRate, cfg.acquisition.frame_rate_hz, "Hz")
                                        : fmt::format("{:.4g} Hz (external trigger; commanded pulse rate)",
                                                      cfg.acquisition.frame_rate_hz)) +
                "\ntrigger: " + (cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal
                                     ? "external (" + cfg.acquisition.trigger.source_entry + ")"
                                     : "freerun") +
                // Whether every pixel in this file was resampled by the camera's
                // wavelength/smile/keystone correction (manual p.32-33) — and
                // therefore which calibration pack applies to it (p.44).
                "\nimage enhancement (AIE): UNKNOWN (node not read back; configuration requests on)" +
                "\nspectral calibration: " + (cfg.recording.wavelengths.source == fx10::WavelengthSource::kNone
                    ? "UNCALIBRATED DN; image rows are not validated spectral bands"
                    : "operator-supplied profile; geometry/serial checked, physical calibration not independently verified") +
                // Not written by this driver: switching it would invalidate the
                // factory BlackLevelOffset and image corrections (manual p.22).
                "\nreadout mode: camera factory setting (interleave untouched)" +
                "\nstatus line (final readback verified): " + (cfg.acquisition.status_line ? "on (REPLACES the last image row)" : "off") +
                (cfg.acquisition.mroi.enabled
                     ? "\nmroi: " + cfg.acquisition.mroi.multiband_string
                     : "") +
                "\nline identity: segment_NNNN.lines.csv; trigger association requires a verified anchor" +
                (cfg.sensor_trigger.enabled ? "\ntiming observations: sensor_trigger.log (SensorSync-Logger)" : "");
    } catch (const fx10::ConfigError &e) {
        // A configuration value only the camera could reject: the wavelength
        // axis against the camera's band count, or an MROI role group the
        // camera does not expose.
        g_log.Error("{}", e.what());
        return abandon(1);
    } catch (const std::exception &e) {
        // TransportError / ControlError
        g_log.Error("{}", e.what());
        return abandon(2);
    }

    return BringUp::kOk;
}


bool Fx10DriverApp::StartStreaming() {
    auto &cfg = impl_->config;
    auto &s = *impl_->session;
    // Per-session outcome state; without the reset a clean session after a
    // reconnect would inherit the previous session's exit code
    impl_->stop_reason = "completed";
    impl_->last_exit_code = 0;
    impl_->retryable_link_loss = false;

    s.recorder = std::make_unique<fx10::EnviRecorder>(cfg.recording, s.counters);
    try {
        s.recorder->Start(s.recorder_init);
    } catch (const fx10::RecorderError &e) {
        g_log.Error("{}", e.what());
        impl_->last_exit_code = 20;
        TeardownSession();
        return false;
    }

    fx10::ExpectedGeometry expected;
    expected.width = static_cast<std::uint32_t>(s.geometry.width);
    expected.height = static_cast<std::uint32_t>(s.geometry.height);
    expected.bytes_per_pixel = s.bytes_per_pixel;
    expected.payload_size = s.geometry.payload_size;
    expected.pixel_format = cfg.acquisition.pixel_format;
    expected.status_line = cfg.acquisition.status_line;

    try {
        s.receiver.Start(*s.recorder, expected, cfg.acquisition.frame_rate_hz);
    } catch (const fx10::TransportError &e) {
        g_log.Error("{}", e.what());
        impl_->last_exit_code = 3;
        impl_->stop_reason = "start-failed";
        TeardownSession();
        return false;
    }

    // Arm the camera before requesting pulses. This ordering alone does not
    // prove a one-to-one frame association (freerun = trigger channel off).
    if (s.trigger_log) {
        const double pulse_hz = cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal
                                    ? cfg.acquisition.frame_rate_hz
                                    : 0.0;
        try {
            s.trigger_log->Start(s.recorder->SessionDir() / "sensor_trigger.log",
                                 {{cfg.sensor_trigger.trigger_channel, pulse_hz}});
        } catch (const fx10::TriggerLogError &e) {
            g_log.Error("{}", e.what());
            impl_->last_exit_code = 20;
            impl_->stop_reason = "trigger-log-start-failed";
            TeardownSession();
            return false;
        }
    }

    // Counter1 binding and reset are optional writes: a baseline read keeps the delta correct when
    // the reset did not happen; -1 = unknown renders as missed_triggers=n/a
    s.missed_baseline = s.control->TryGetInt(fx10::node::kMissedTriggerCount).value_or(-1);
    return true;
}


void Fx10DriverApp::MonitorLoop() {
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
    std::optional<std::int64_t> missed;
    std::optional<double> temp_pcb;
    std::optional<double> temp_fpga;
    bool over_temperature = false;

    // A freely controlled external trigger may intentionally pause. SensorSync
    // is different: this process commanded periodic pulses, so silence is a fault.
    const bool watchdog_applies = cfg.acquisition.trigger.mode == fx10::TriggerMode::kFreerun ||
                                  cfg.sensor_trigger.enabled; // this process commanded periodic pulses

    while (true) {
        // Republish the receiver's liveness for Main. impl_->session is owned by
        // THIS thread (the reconnect loop resets it), so Main must never reach
        // into it; it reads this atomic instead.
        data_reference_us_.store(
            watchdog_applies ? s.receiver.LastDataReferenceUs().value_or(0) : 0, std::memory_order_release);

        if (StopRequested()) {
            impl_->stop_reason = "external-stop";
            break;
        }
        if (s.receiver.Failed() || (s.recorder && s.recorder->Failed())) {
            break; // classified in teardownSession_
        }
        if (s.trigger_log && !s.trigger_log->Ok()) {
            g_log.Error("Sensor trigger log/protocol integrity failed (I/O, rejected command, restart or event loss); stopping");
            impl_->stop_reason = "trigger-log-failure";
            impl_->last_exit_code = 20;
            break;
        }
        if (s.trigger_log && s.trigger_log->StalledSeconds() > kTriggerLogStallAbortS) {
            // Even without an explicit I/O/protocol error, sustained silence
            // means timing observations are unavailable.
            g_log.Error("Sensor trigger timing log has not grown for {:.0f} s (Teensy USB link lost?) — "
                        "frames without timing are worthless, stopping",
                        s.trigger_log->StalledSeconds());
            impl_->stop_reason = "trigger-log-stalled";
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
            // (the standalone main-thread pattern; the acquisition thread owns
            // the rest of the Counters ledger while streaming)
            missed = s.control->TryGetInt(fx10::node::kMissedTriggerCount);
            temp_pcb = s.control->TryReadTemperature("ProcPCB");
            temp_fpga = s.control->TryReadTemperature("FPGA");

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
            const std::optional<std::int64_t> missed_delta =
                    missed && s.missed_baseline >= 0
                        ? std::optional<std::int64_t>(*missed - s.missed_baseline)
                        : std::nullopt;
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


void Fx10DriverApp::TeardownSession() {
    // Disarm Main's no-data watchdog first: from here on there is no session,
    // and the reconnect gap that may follow is silence this driver expects.
    data_reference_us_.store(0, std::memory_order_release);
    if (!impl_->session) {
        return;
    }
    auto &s = *impl_->session;

    // Stop pulses first, then the stream and counter reads. A strobe already
    // active at STOP may lack its ending edge; the offline index must flag it.
    bool trigger_log_failed = false;
    if (s.trigger_log) {
        s.trigger_log->Stop();
        trigger_log_failed = !s.trigger_log->Ok();
    }
    s.receiver.Stop();

    if (s.missed_baseline >= 0 && s.control) {
        const auto missed_final = s.control->TryGetInt(fx10::node::kMissedTriggerCount);
        s.counters.missed_trigger_delta = missed_final ? *missed_final - s.missed_baseline : -1;
    }

    int exit_code = impl_->last_exit_code;
    if (s.receiver.Failed()) {
        switch (s.receiver.GetFatalKind()) {
            case fx10::StreamReceiver::FatalKind::kLinkLost:
                exit_code = 21;
                impl_->stop_reason = "link-loss";
                impl_->retryable_link_loss = true;
                break;
            case fx10::StreamReceiver::FatalKind::kFirstFrame:
                exit_code = 3;
                impl_->stop_reason = "first-frame-mismatch";
                break;
            case fx10::StreamReceiver::FatalKind::kStreamUnusable:
                // The link is up but the data is not usable: a reconnect is the
                // right response, same as a link loss.
                exit_code = 21;
                impl_->stop_reason = "stream-unusable";
                impl_->retryable_link_loss = true;
                break;
            case fx10::StreamReceiver::FatalKind::kSinkFailed:
                break; // the recorder branch below carries the real cause
            default:
                exit_code = 3;
                impl_->stop_reason = "transport-error";
                break;
        }
    }
    if (s.recorder) {
        // Finalize FIRST: fdatasync / rename / .hdr failures are raised inside
        // Stop(), so classifying before it would miss exactly the failures that
        // leave a segment without its header.
        s.recorder->Stop(impl_->stop_reason);
    }
    if (s.recorder && s.recorder->Failed()) {
        // Classified by kind, not by matching substrings of the message.
        exit_code = s.recorder->GetErrorKind() == fx10::ErrorKind::kIo ? 20 : 10;
        impl_->stop_reason = "recorder-failure";
        impl_->retryable_link_loss = false;
    }
    if (trigger_log_failed) {
        // A timing-log failure (disk full) outranks a concurrent link loss: the
        // frames have no time source, so a reconnect must not resurrect the run.
        exit_code = 20;
        impl_->stop_reason = "trigger-log-failure";
        impl_->retryable_link_loss = false;
    }

    if (s.recorder) {
        if (exit_code == 0 && fx10::Classify(s.counters) == fx10::RunStatus::kDegraded) {
            exit_code = 10;
        }
        fx10::FinalStatsSample sample;
        sample.frames = s.counters.frames_written;
        sample.frames_missed_rx = s.counters.frames_missed_rx;
        // < 0 = the counter was never read successfully this session
        sample.missed_triggers = s.counters.missed_trigger_delta >= 0
                                     ? std::optional<std::int64_t>(s.counters.missed_trigger_delta)
                                     : std::nullopt;
        sample.retrieve_timeouts = s.counters.retrieve_timeouts;
        g_log.Info("{}", fx10::FormatFinalStatsLine(sample));
    }
    impl_->last_exit_code = exit_code;
    impl_->run_incomplete = impl_->run_incomplete || exit_code != 0;

    s.receiver.Disconnect();
    impl_->session.reset();
}


void Fx10DriverApp::Run() {
    // Consecutive failed reconnects, reset by every session that streams.
    int attempts = 0;
    while (!StopRequested()) {
        if (!impl_->session) {
            // Reconnect path only; the first session comes from Init()
            if (attempts >= impl_->config.network.reconnect.max_attempts) {
                g_log.Error("Connect Failed: {} reconnect attempts exhausted", attempts);
                break;
            }
            ++attempts;
            g_log.Warn("Link lost — reconnect attempt {}/{} in {} ms (new connection epoch)", attempts,
                       impl_->config.network.reconnect.max_attempts, impl_->config.network.reconnect.backoff_ms);
            SleepInterruptible(impl_->config.network.reconnect.backoff_ms);
            if (StopRequested()) break;
            const BringUp bring_up = BringUpSession();
            if (bring_up == BringUp::kStopped) {
                break; // clean external stop during the reconnect: not a failure
            }
            if (bring_up != BringUp::kOk) {
                impl_->run_incomplete = true;
                if (impl_->last_exit_code == 1) break; // a rejected calibration/configuration is not a transient link fault
                continue; // each failed bring-up consumes exactly one attempt
            }
        }
        if (!StartStreaming()) {
            break;
        }
        attempts = 0; // this session reached the streaming state
        MonitorLoop();
        TeardownSession();
        if (StopRequested() || !impl_->retryable_link_loss || !impl_->config.network.reconnect.enabled) {
            break;
        }
    }
    // Any exit (internal failure, limits, external stop) takes the whole rig down
    terminate_.store(true, std::memory_order_release);
}


std::optional<std::uint64_t> Fx10DriverApp::MicrosSinceLastData() const {
    const auto reference = data_reference_us_.load(std::memory_order_acquire);
    if (reference == 0) {
        return std::nullopt;
    }
    const auto now = common::TimeUtil::SteadyNowUs();
    return now > reference ? now - reference : 0;
}


void Fx10DriverApp::Shutdown() {
    data_reference_us_.store(0, std::memory_order_release);
    if (shutdown_called_.exchange(true)) {
        return;
    }

    terminate_.store(true, std::memory_order_release);

    // Only reachable when Run() never executed (init OK but the rig bring-up was
    // interrupted): connected but not streaming. teardownSession_ is safe there.
    if (impl_ && impl_->session) {
        TeardownSession();
    }

    if (!init_ok_) {
        return; // the unified main already reported the init failure
    }
    if (impl_->last_exit_code == 0 && !impl_->run_incomplete) {
        g_log.Info("{}", common::Markers::kFx10Shutdown);
    } else {
        MarkFailed();
        g_log.Error("{} (last session exit code {}; earlier session failures remain latched)", common::Markers::kFx10SessionIssues,
                   impl_->last_exit_code);
    }
}
