#include "session_internal.h"

#include <utility>
#include "driver_markers.h"
#include "ebus/env_bootstrap.h"
#include "sensor_sync_quality.h"
#include "stats_line.h"
#include "time_util.h"

namespace fx10 {
namespace session_detail {
    common::DriverLog g_log{std::string(common::Markers::kModuleFx10)};
}
using namespace session_detail;

Session::Session(std::filesystem::path config_path, std::filesystem::path data_folder_path,
                 std::shared_ptr<common::SensorSyncHub> sync, std::function<bool()> stop_requested)
    : config_path_(std::move(config_path)), data_folder_path_(std::move(data_folder_path)),
      external_stop_(std::move(stop_requested)), impl_(std::make_unique<Impl>()) {
    impl_->sync = std::move(sync);
}


Session::~Session() {
    Shutdown();
}


bool Session::StopRequested() const {
    return shutdown_requested_.load(std::memory_order_acquire) || (external_stop_ && external_stop_());
}


bool Session::Prepare() {
    // Load + validate the driver YAML
    try {
        impl_->config = fx10::LoadAppConfig(config_path_);
    } catch (const fx10::ConfigError &e) {
        impl_->run_incomplete = true;
        impl_->final_statistics["status"] = "config_error";
        impl_->final_statistics["error"] = e.what();
        g_log.Error("FX10 config error: {}", e.what());
        return false;
    }

    impl_->config.recording.output_dir = data_folder_path_ / "fx10";
    std::error_code ec;
    std::filesystem::create_directories(impl_->config.recording.output_dir, ec);
    if (ec) {
        impl_->run_incomplete = true;
        impl_->final_statistics["status"] = "output_directory_error";
        impl_->final_statistics["error"] = ec.message();
        g_log.Error("FX10 cannot create output directory: {}", ec.message());
        return false;
    }

    // GenICam environment
    common::Ebus::BootstrapEnv();

    switch (BringUpSession()) {
        case BringUp::kStopped:
            impl_->final_statistics["status"] = "startup_interrupted";
            g_log.Warn("FX10 bring-up interrupted by shutdown request");
            return false;
        case BringUp::kFailed:
            g_log.Error("FX10 startup failed (standalone exit code {})", impl_->last_exit_code);
            return false;
        case BringUp::kOk:
            break;
    }

    return true;
}


void Session::TeardownSession() {
    // Disarm Main's no-data watchdog first: from here on there is no session,
    // and the silence that follows is expected.
    data_reference_us_.store(0, std::memory_order_release);
    if (!impl_->session) {
        return;
    }
    auto &s = *impl_->session;

    // STOP halts new pulses, retains in-flight exposure edges, then closes the
    // timing log. Independently check the actual tail, including older firmware.
    bool trigger_log_failed = false;
    if (impl_->sync_participant) {
        impl_->sync->Disarm(kSensorSyncOwner); // first disarm ends the rig's session: pulses stop, log closes
        trigger_log_failed = !impl_->sync->Ok();
    }
    s.receiver.Stop();

    if (s.telemetry.IsOpen()) {
        s.RecordTelemetry(s.acquisition_started ? "stop" : "startup-failed");
        s.counters.missed_trigger_delta = fx10::MissedTriggerDelta(s.latest_telemetry).value_or(-1);
        try { s.telemetry.Close(); }
        catch (const fx10::MetadataError &e) {
            s.metadata_failed = true;
            g_log.Error("Telemetry close failed: {}", e.what());
        }
        if (!s.receiver.DumpStreamParams(s.recorder->SessionDir() / "stream_stats.txt")) s.metadata_failed = true;
    }

    int exit_code = impl_->last_exit_code;
    nlohmann::ordered_json timing_quality = {{"status", impl_->sync_participant ? "not_started" : "disabled"},
                                            {"association_verified", false}};
    if (s.acquisition_started && impl_->sync_participant && !impl_->sync->Started() && exit_code == 0) {
        exit_code = 10;
        impl_->stop_reason = "trigger-session-not-started";
        impl_->last_error = "acquisition started but the required SensorSync timing session did not start";
    }
    if (s.receiver.Failed()) {
        switch (s.receiver.GetFatalKind()) {
            case fx10::StreamReceiver::FatalKind::kLinkLost:
                exit_code = 21;
                impl_->stop_reason = "link-loss";
                break;
            case fx10::StreamReceiver::FatalKind::kFirstFrame:
                exit_code = 3;
                impl_->stop_reason = "first-frame-mismatch";
                break;
            case fx10::StreamReceiver::FatalKind::kStreamUnusable:
                // The link is up but the data is not usable; classified like a
                // link loss (no reconnect: the recording is already incomplete).
                exit_code = 21;
                impl_->stop_reason = "stream-unusable";
                break;
            case fx10::StreamReceiver::FatalKind::kSinkFailed:
                break; // the recorder branch below carries the real cause
            case fx10::StreamReceiver::FatalKind::kRecordingPipeline:
                exit_code = 10;
                impl_->stop_reason = "recording-pipeline-failure";
                break; // a recorder I/O error below takes precedence
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
    if (s.recorder && impl_->sync_participant && impl_->sync->Started()) {
        try {
            const auto quality = common::InspectSensorSync(impl_->sync->LogPath(),
                static_cast<unsigned>(impl_->config.sensor_trigger.trigger_channel),
                impl_->config.acquisition.trigger.mode == fx10::TriggerMode::kExternal,
                s.recorder->FramesWrittenTotal(), s.counters.frames_missed_rx);
            timing_quality = quality.Json();
            fx10::PublishMetadata(s.recorder->SessionDir() / "timing_quality.json", quality.Json());
            if (!quality.Ok()) {
                g_log.Error("Trigger/exposure/frame accounting or exposure tail is incomplete: {}",
                            quality.Json().dump());
                if (exit_code == 0) {
                    exit_code = 10;
                    impl_->stop_reason = "trigger-exposure-accounting";
                }
            }
        } catch (const std::exception &e) {
            timing_quality["status"] = "inspection_or_publication_failed";
            timing_quality["error"] = e.what();
            impl_->last_error = e.what();
            s.metadata_failed = true;
            g_log.Error("Cannot assess/persist timing accounting: {}", e.what());
        }
    }
    if (s.recorder && s.recorder->Failed()) {
        // Classified by kind, not by matching substrings of the message.
        exit_code = s.recorder->GetErrorKind() == fx10::ErrorKind::kIo ? 20 : 10;
        impl_->stop_reason = "recorder-failure";
    }
    if (trigger_log_failed) {
        // A timing-log failure (disk full) outranks a concurrent link loss: the
        // frames have no time source.
        exit_code = 20;
        impl_->stop_reason = "trigger-log-failure";
    }
    if (s.counter_binding_verified && s.counter_regressed && exit_code == 0) {
        exit_code = 10;
        impl_->stop_reason = "counter-discontinuity";
    }
    if (s.metadata_failed) {
        exit_code = 20;
        impl_->stop_reason = "metadata-failure";
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
    // Preserve the native ledger and cause before destroying this connection
    // epoch. Main asks for this only after Run and Shutdown have completed.
    impl_->final_statistics = {{"schema_version", 1},
        {"status", exit_code != 0 ? "failed" : s.acquisition_started ? "completed" : "not_started"},
        {"exit_code", exit_code}, {"stop_reason", impl_->stop_reason}, {"error", impl_->last_error},
        {"acquisition_started", s.acquisition_started}, {"run_incomplete", impl_->run_incomplete},
        {"frames_admitted", s.receiver.FramesDelivered()}, {"counters", fx10::FinalCountersJson(s.counters)},
        {"transport_failed", s.receiver.Failed()}, {"transport_error", s.receiver.ErrorMessage()},
        {"recorder_failed", s.recorder && s.recorder->Failed()},
        {"recorder_error", s.recorder ? s.recorder->ErrorMessage() : std::string()},
        {"metadata_failed", s.metadata_failed}, {"trigger_log_failed", trigger_log_failed},
        {"final_telemetry", fx10::BuildTelemetryJson(s.latest_telemetry, "final")},
        {"sensor_sync", std::move(timing_quality)}};

    s.receiver.Disconnect();
    impl_->session.reset();
}


void Session::Run() {
    // One connection epoch per run: a link loss, like any data loss, ends the
    // whole rig (fail-fast) instead of reconnecting into a recording that is
    // already incomplete. The connection comes from Prepare().
    if (!StopRequested() && impl_->session && StartStreaming()) {
        MonitorLoop();
        TeardownSession();
    }
    // The IDriverApp adapter decides how this session's completion stops the rig.
}


std::optional<std::uint64_t> Session::MicrosSinceLastData() const {
    const auto reference = data_reference_us_.load(std::memory_order_acquire);
    if (reference == 0) {
        return std::nullopt;
    }
    const auto now = common::TimeUtil::SteadyNowUs();
    return now > reference ? now - reference : 0;
}

nlohmann::ordered_json Session::FinalStatistics() const {
    return impl_->final_statistics;
}


void Session::Shutdown() {
    data_reference_us_.store(0, std::memory_order_release);
    if (shutdown_called_.exchange(true)) {
        return;
    }

    shutdown_requested_.store(true, std::memory_order_release);

    // Also handles Prepare() followed by cancellation before Run(), or a
    // Run() exception. TeardownSession preserves the same ordered drain.
    if (impl_ && impl_->session) {
        TeardownSession();
    }
}


bool Session::Failed() const {
    return impl_->run_incomplete || impl_->last_exit_code != 0;
}

} // namespace fx10
