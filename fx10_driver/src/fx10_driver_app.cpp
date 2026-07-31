#include "fx10_driver_app.h"

#include <chrono>
#include <filesystem>
#include <limits>
#include <spdlog/spdlog.h>
#include <string_view>
#include <thread>

#include "app_config.hpp"
#include "envi_recorder.hpp"
#include "pixel_format.hpp"
#include "logger.h"
#include "wavelengths.hpp"
#include "driver_markers.h"
#include "ebus/camera_control.hpp"
#include "ebus/ebus_env.hpp"
#include "ebus/stream_receiver.hpp"
#include "time_util.h"
#include "utility.h"


namespace {
    Common::DriverLog g_log{"FX10"};

    // Free space in GB at `dir` (must exist); -1 when unknowable
    double freeDiskGb(const std::filesystem::path &dir) {
        std::error_code ec;
        const auto info = std::filesystem::space(dir, ec);
        return ec ? -1.0 : static_cast<double>(info.available) / 1e9;
    }
}


// One Session per camera connection
struct Fx10DriverApp::Impl {
    struct Session {
        fx10::Counters counters;
        fx10::StreamReceiver receiver;
        std::unique_ptr<fx10::CameraControl> control; // built after connect
        std::unique_ptr<fx10::EnviRecorder> recorder; // built in startStreaming_
        fx10::RecorderInit recorder_init;
        fx10::CameraControl::Geometry geometry;
        std::uint32_t bytes_per_pixel = 2;
        std::int64_t missed_baseline = -1;

        explicit Session(const fx10::NetworkConfig &network) : receiver(network, counters) {
        }
    };

    fx10::Config config;
    std::unique_ptr<Session> session;

    std::string stop_reason = "completed"; // logged by EnviRecorder::stop as the exit reason
    int last_exit_code = 0; // standalone exit-code semantics, reported at shutdown
    bool retryable_link_loss = false;
};


Fx10DriverApp::Fx10DriverApp(const Common::Config &config) : impl_(std::make_unique<Impl>()) {
    // Actual config loading is deferred to init()
    std::filesystem::path exe_dir = Common::GetExecutableDir(); // exe_dir + "../../" -> project root
    config_path_ = exe_dir / "../../" / config.fx10_config_path;
    data_folder_path_ = config.data_folder_path;
}


Fx10DriverApp::~Fx10DriverApp() {
    shutdown();
}


bool Fx10DriverApp::stopRequested_() const {
    return terminate_.load(std::memory_order_acquire) || (external_stop_ && external_stop_());
}


void Fx10DriverApp::sleepInterruptible_(int total_ms) {
    while (total_ms > 0 && !stopRequested_()) {
        const int slice = total_ms < 100 ? total_ms : 100;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        total_ms -= slice;
    }
}


bool Fx10DriverApp::init(const std::function<bool()> &external_stop) {
    external_stop_ = external_stop;

    // Load + validate the driver YAML
    try {
        impl_->config = fx10::Config::loadFromFile(config_path_);
    } catch (const fx10::ConfigError &e) {
        Common::Log::log_and_throw(Common::Markers::kModuleFx10, "FX10 config error", e.what(), false);
        return false;
    }

    // ENVI sessions land in <data_folder>/bin/fx10/<base>_<UTC>Z/
    impl_->config.recording.output_dir = data_folder_path_ + "/bin/fx10";
    std::error_code ec;
    std::filesystem::create_directories(impl_->config.recording.output_dir, ec);
    if (ec) {
        Common::Log::log_and_throw(Common::Markers::kModuleFx10, "FX10 cannot create output directory", ec.message(),
                                   false);
        return false;
    }

    // GenICam environment
    fx10::bootstrapGenicamEnv();

    switch (bringUpSession_()) {
        case BringUp::kStopped:
            Common::Log::log_message(spdlog::level::warn, Common::Markers::kModuleFx10,
                                     "FX10 bring-up interrupted by shutdown request");
            return false;
        case BringUp::kFailed:
            Common::Log::log_and_throw(
                Common::Markers::kModuleFx10,
                fmt::format("FX10 startup failed (standalone exit code {})", impl_->last_exit_code), "", false);
            return false;
        case BringUp::kOk:
            break;
    }

    Common::Log::log_message(spdlog::level::info, Common::Markers::kModuleFx10, Common::Markers::kFx10Initialized);
    init_ok_ = true;
    return true;
}


Fx10DriverApp::BringUp Fx10DriverApp::bringUpSession_() {
    auto &cfg = impl_->config;
    impl_->session = std::make_unique<Impl::Session>(cfg.network);
    auto &s = *impl_->session;

    // The eBUS calls below block without a cancellation API
    const auto stopped = [this, &s]() {
        if (!stopRequested_()) {
            return false;
        }
        s.receiver.disconnect();
        impl_->session.reset();
        return true;
    };

    try {
        if (stopped()) {
            return BringUp::kStopped;
        }
        s.receiver.connect(cfg.device);
        if (stopped()) {
            return BringUp::kStopped;
        }
        s.receiver.openStream();

        s.control = std::make_unique<fx10::CameraControl>(*s.receiver.device(), cfg.features);
        s.control->applyAcquisitionConfig(cfg.acquisition);
        s.geometry = s.control->readGeometry();
        if (stopped()) {
            return BringUp::kStopped;
        }

        const int expected_bands = cfg.acquisition.expectedBands();
        if (s.geometry.height != expected_bands) {
            g_log.warn(
                "Camera reports {} bands, config implies {} (MROI margins or camera window?) — the camera value is authoritative",
                s.geometry.height, expected_bands);
        }

        const auto try_get_string = [&s](const char *node) {
            try {
                return s.control->getString(node);
            } catch (const fx10::ControlError &) {
                return std::string("?");
            }
        };

        // Storage layout after the receiver's unpack step (packed formats land
        // on disk as canonical uint16 — the ENVI contract never sees packing)
        const auto *pf = fx10::pixelFormatInfo(cfg.acquisition.pixel_format);
        s.bytes_per_pixel = pf != nullptr ? pf->storage_bpp : 2;
        s.recorder_init.samples = static_cast<std::uint32_t>(s.geometry.width);
        s.recorder_init.bands = static_cast<std::uint32_t>(s.geometry.height);
        s.recorder_init.bytes_per_pixel = s.bytes_per_pixel;
        s.recorder_init.data_type = s.bytes_per_pixel == 1 ? fx10::EnviDataType::kUint8 : fx10::EnviDataType::kUint16;
        s.recorder_init.tick_frequency_hz =
                static_cast<std::uint64_t>(s.control->tryGetIntByRole("timestamp_tick_frequency").value_or(0));
        s.recorder_init.wavelengths =
                fx10::resolveWavelengths(cfg.recording.wavelengths, static_cast<int>(s.geometry.height));
        s.recorder_init.description =
                "vendor: " + try_get_string("DeviceVendorName") +
                "\nmodel: " + try_get_string("DeviceModelName") +
                "\nserial: " + try_get_string("DeviceSerialNumber") +
                "\npixel format: " + s.geometry.pixel_format +
                "\nspatial binning: " + std::to_string(cfg.acquisition.spatial_binning) +
                "\nspectral binning: " + std::to_string(cfg.acquisition.spectral_binning) +
                "\nexposure_ms: " + std::to_string(cfg.acquisition.exposure_ms) +
                "\ntrigger: " + (cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal
                                     ? "external (" + cfg.acquisition.trigger.source_entry + ")"
                                     : "freerun " + std::to_string(cfg.acquisition.frame_rate_hz) + " Hz");

        const double disk_free = freeDiskGb(cfg.recording.output_dir);
        if (disk_free >= 0.0 && disk_free < cfg.disk.min_free_gb) {
            g_log.error("Only {:.1f} GB free under '{}' (hard floor {} GB)", disk_free, cfg.recording.output_dir,
                        cfg.disk.min_free_gb);
            impl_->last_exit_code = 20;
            s.receiver.disconnect();
            impl_->session.reset();
            return BringUp::kFailed;
        }
    } catch (const fx10::ConfigError &e) {
        // resolveWavelengths
        g_log.error("{}", e.what());
        impl_->last_exit_code = 1;
        impl_->session->receiver.disconnect();
        impl_->session.reset();
        return BringUp::kFailed;
    } catch (const std::exception &e) {
        // TransportError / ControlError
        g_log.error("{}", e.what());
        impl_->last_exit_code = 2;
        impl_->session->receiver.disconnect();
        impl_->session.reset();
        return BringUp::kFailed;
    }

    return BringUp::kOk;
}


bool Fx10DriverApp::startStreaming_() {
    auto &cfg = impl_->config;
    auto &s = *impl_->session;
    // Per-session outcome state; without the reset a clean session after a
    // reconnect would inherit the previous session's exit code
    impl_->stop_reason = "completed";
    impl_->last_exit_code = 0;
    impl_->retryable_link_loss = false;

    s.recorder = std::make_unique<fx10::EnviRecorder>(cfg.recording, s.counters);
    try {
        s.recorder->start(s.recorder_init);
    } catch (const fx10::RecorderError &e) {
        g_log.error("{}", e.what());
        impl_->last_exit_code = 20;
        teardownSession_();
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
        s.receiver.start(*s.recorder, expected, cfg.acquisition.frame_rate_hz, cfg.watchdog,
                         cfg.resolvedNoFrameAbortS());
    } catch (const fx10::TransportError &e) {
        g_log.error("{}", e.what());
        impl_->last_exit_code = 3;
        impl_->stop_reason = "start-failed";
        teardownSession_();
        return false;
    }

    // Counter1 auto-resets at AcquisitionStart on the FX10e, but a baseline
    // keeps this correct for cameras/configs where it does not
    s.missed_baseline = s.control->tryGetIntByRole("missed_trigger_count").value_or(-1);
    return true;
}


void Fx10DriverApp::monitorLoop_() {
    using clock = std::chrono::steady_clock;
    auto &cfg = impl_->config;
    auto &s = *impl_->session;

    const auto t_start = clock::now();
    const double stats_interval = cfg.logging.stats_interval_s;
    auto next_stats = t_start + std::chrono::duration_cast<clock::duration>(
                          std::chrono::duration<double>(stats_interval > 0 ? stats_interval : 3600.0));

    while (true) {
        if (stopRequested_()) {
            impl_->stop_reason = "external-stop";
            break;
        }
        if (s.receiver.failed() || (s.recorder && s.recorder->failed())) {
            break; // classified in teardownSession_
        }
        const double elapsed = std::chrono::duration<double>(clock::now() - t_start).count();
        if (cfg.recording.max_duration_s > 0.0 && elapsed >= cfg.recording.max_duration_s) {
            impl_->stop_reason = "duration-reached";
            break;
        }
        if (cfg.recording.max_frames > 0 && s.receiver.framesDelivered() >= cfg.recording.max_frames) {
            impl_->stop_reason = "frame-count-reached";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (stats_interval > 0 && clock::now() >= next_stats) {
            next_stats = clock::now() +
                         std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(stats_interval));
            // Thread-safe sources only: atomics + GenICam control-channel reads
            // (the standalone main-thread pattern; the acquisition thread owns
            // the rest of the Counters ledger while streaming)
            const auto missed = s.control->tryGetIntByRole("missed_trigger_count");
            const auto temperature = s.control->tryGetFloatByRole("device_temperature");
            const double free_gb = freeDiskGb(cfg.recording.output_dir);
            const double missed_delta = missed && s.missed_baseline >= 0
                                            ? static_cast<double>(*missed - s.missed_baseline)
                                            : std::numeric_limits<double>::quiet_NaN();
            g_log.info("[Statistics] frames={}  missed_triggers={}  temp={:.4f} °C  disk_free={:.1f} GB",
                       s.receiver.framesDelivered(), missed_delta,
                       temperature.value_or(std::numeric_limits<double>::quiet_NaN()), free_gb);
            if (free_gb >= 0.0 && free_gb < cfg.disk.min_free_gb) {
                g_log.error("Disk free {:.1f} GB below hard floor {} GB — stopping cleanly", free_gb,
                            cfg.disk.min_free_gb);
                impl_->stop_reason = "disk-floor";
                impl_->last_exit_code = 20;
                break;
            }
            if (free_gb >= 0.0 && free_gb < cfg.disk.warn_free_gb) {
                g_log.warn("Disk free {:.1f} GB below warning floor {} GB", free_gb, cfg.disk.warn_free_gb);
            }
        }
    }
}


void Fx10DriverApp::teardownSession_() {
    if (!impl_->session) {
        return;
    }
    auto &s = *impl_->session;

    // Order matters: stop the stream first, then the final counter reads
    s.receiver.stop();

    if (s.missed_baseline >= 0 && s.control) {
        const auto missed_final = s.control->tryGetIntByRole("missed_trigger_count");
        s.counters.missed_trigger_delta = missed_final ? *missed_final - s.missed_baseline : -1;
    }

    int exit_code = impl_->last_exit_code;
    if (s.receiver.failed()) {
        switch (s.receiver.fatalKind()) {
            case fx10::StreamReceiver::FatalKind::kLinkLost:
                exit_code = 21;
                impl_->stop_reason = "link-loss";
                impl_->retryable_link_loss = true;
                break;
            case fx10::StreamReceiver::FatalKind::kWatchdog:
                exit_code = 22;
                impl_->stop_reason = "watchdog-abort";
                break;
            case fx10::StreamReceiver::FatalKind::kFirstFrame:
                exit_code = 3;
                impl_->stop_reason = "first-frame-mismatch";
                break;
            case fx10::StreamReceiver::FatalKind::kSinkFailed:
                break; // the recorder branch below carries the real cause
            default:
                exit_code = 3;
                impl_->stop_reason = "transport-error";
                break;
        }
    }
    if (s.recorder && s.recorder->failed()) {
        const std::string &message = s.recorder->errorMessage();
        const bool io_failure = message.find("write failed") != std::string::npos ||
                                message.find("finalize sync") != std::string::npos ||
                                message.find("rotation failed") != std::string::npos;
        exit_code = io_failure ? 20 : 10;
        impl_->stop_reason = "recorder-failure";
    }

    if (s.recorder) {
        s.recorder->stop(impl_->stop_reason);
        if (exit_code == 0 && fx10::classify(s.counters) == fx10::RunStatus::kDegraded) {
            exit_code = 10;
        }
        const double free_gb = freeDiskGb(impl_->config.recording.output_dir);
        g_log.info("[STATISTICS] Final Stats: frames={}  frames_missed_rx={}  missed_triggers={}  disk_free={:.1f} GB",
                   s.counters.frames_written, s.counters.frames_missed_rx, s.counters.missed_trigger_delta,
                   free_gb);
    }
    impl_->last_exit_code = exit_code;

    s.receiver.disconnect();
    impl_->session.reset();
}


void Fx10DriverApp::run() {
    int attempts = 0;
    while (!stopRequested_()) {
        if (!impl_->session) {
            // Reconnect path only; the first session comes from init()
            if (bringUpSession_() != BringUp::kOk) {
                impl_->last_exit_code = 21; // link loss never resolved
                break;
            }
        }
        if (!startStreaming_()) {
            break;
        }
        monitorLoop_();
        teardownSession_();
        if (stopRequested_() || !impl_->retryable_link_loss || !impl_->config.network.reconnect.enabled) {
            break;
        }
        if (++attempts > impl_->config.network.reconnect.max_attempts) {
            g_log.error("Connect Failed: {} reconnect attempts exhausted", attempts - 1);
            break; // last_exit_code stays 21
        }
        g_log.warn("Link lost — reconnect attempt {}/{} in {} ms (new session directory)", attempts,
                   impl_->config.network.reconnect.max_attempts, impl_->config.network.reconnect.backoff_ms);
        sleepInterruptible_(impl_->config.network.reconnect.backoff_ms);
    }
    // Any exit (internal failure, limits, external stop) takes the whole rig down
    terminate_.store(true, std::memory_order_release);
}


void Fx10DriverApp::shutdown() {
    if (shutdown_called_.exchange(true)) {
        return;
    }

    terminate_.store(true, std::memory_order_release);

    // Only reachable when run() never executed (init OK but the rig bring-up was
    // interrupted): connected but not streaming. teardownSession_ is safe there.
    if (impl_ && impl_->session) {
        teardownSession_();
    }

    if (!init_ok_) {
        return; // the unified main already reported the init failure
    }
    if (impl_->last_exit_code == 0) {
        Common::Log::log_message(spdlog::level::info, Common::Markers::kModuleFx10, Common::Markers::kFx10Shutdown);
    } else {
        Common::Log::log_message(spdlog::level::warn, Common::Markers::kModuleFx10,
                                 fmt::format("{} (standalone exit code {})", Common::Markers::kFx10SessionIssues,
                                             impl_->last_exit_code));
    }
}
