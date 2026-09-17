#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "accounting.h"
#include "app_config.h"
#include "ebus/camera_control.h"
#include "ebus/env_bootstrap.h"
#include "ebus/stream_receiver.h"
#include "envi_recorder.h"
#include "logger.h"
#include "pixel_format.h"
#include "sensor_trigger_log.h"
#include "session_metadata.h"
#include "time_util.h"
#include "wavelengths.h"

namespace {
    using Clock = std::chrono::steady_clock;
    volatile std::sig_atomic_t g_signal = 0;
    void OnSignal(int signal) { g_signal = signal; }
    void CheckCancelled() {
        if (g_signal != 0) throw std::runtime_error("reference collection interrupted");
    }
    void Marker(const std::string &message) {
        std::printf("REFERENCE: %s\n", message.c_str());
        std::fflush(stdout);
    }

    // The receiver outlives each phase's sink. Even partial Start failures must
    // drain/join both workers BEFORE the recorder or trigger logger is destroyed.
    struct PhaseStop {
        fx10::StreamReceiver &receiver;
        fx10::EnviRecorder &recorder;
        fx10::SensorTriggerLog *trigger;
        bool done = false;
        void Finish(const std::string &reason) {
            if (done) return;
            if (trigger) trigger->Stop();
            receiver.Stop();
            recorder.Stop(reason);
            done = true;
        }
        ~PhaseStop() {
            try { Finish("reference-incomplete"); } catch (...) {}
        }
    };

    // `sensor_port`: the rig's SensorSync board (config-main.yaml "Sensor Trigger: Port",
    // passed as --sensor-port); this tool runs without main and owns the board alone
    nlohmann::json CapturePhase(const char *phase, fx10::AppConfig cfg, const std::string &sensor_port,
                                const std::filesystem::path &root,
                                fx10::StreamReceiver &receiver, fx10::CameraControl &control,
                                fx10::Counters &counters) {
        CheckCancelled();
        // The previous phase has completely stopped and destroyed its recorder.
        counters = {};
        cfg.recording.output_dir = root / "raw" / "fx10" / phase;
        const auto geometry = control.ReadGeometry();
        const auto *pixel = fx10::GetPixelFormatInfo(cfg.acquisition.pixel_format);
        if (!pixel || geometry.width <= 0 || geometry.height <= 0 ||
            geometry.width > std::numeric_limits<std::uint32_t>::max() ||
            geometry.height > std::numeric_limits<std::uint32_t>::max() ||
            geometry.pixel_format != cfg.acquisition.pixel_format) {
            throw std::runtime_error("reference geometry/pixel format is not usable");
        }
        fx10::RecorderInit init;
        init.samples = static_cast<std::uint32_t>(geometry.width);
        init.bands = static_cast<std::uint32_t>(geometry.height);
        init.bytes_per_pixel = pixel->storage_bpp;
        init.data_type = pixel->storage_bpp == 1 ? fx10::EnviDataType::kUint8 : fx10::EnviDataType::kUint16;
        init.pixel_format = geometry.pixel_format;
        init.expected_frame_rate_hz = cfg.acquisition.frame_rate_hz;
        const auto serial = cfg.recording.wavelengths.source == fx10::WavelengthSource::kNone
                                ? std::string() : control.GetString("DeviceSerialNumber");
        init.wavelengths = fx10::ResolveForGeometry(cfg.recording.wavelengths, serial,
            geometry.width, geometry.height, geometry.offset_x, geometry.offset_y, cfg.acquisition.status_line);
        init.description = std::string("FX10 ") + phase + " reference; requested duration " +
            std::to_string(cfg.reference.duration_s) + " s; "
            "saved acquisition configuration; shutter pulse acknowledgement is not position feedback";

        std::unique_ptr<fx10::SensorTriggerLog> trigger;
        if (cfg.sensor_trigger.enabled) {
            trigger = std::make_unique<fx10::SensorTriggerLog>(sensor_port);
            trigger->Open();
        }
        fx10::EnviRecorder recorder(cfg.recording, counters);
        PhaseStop cleanup{receiver, recorder, trigger.get()};
        recorder.Start(init);
        fx10::JsonlFile telemetry;
        telemetry.Open(recorder.SessionDir() / "telemetry.jsonl");
        std::optional<std::int64_t> missed_baseline;
        bool counter_binding_verified = false;
        const auto record_telemetry = [&](const char *event, bool start) {
            fx10::DeviceTelemetry sample;
            sample.host_realtime_ns = common::TimeUtil::RealtimeNowNs();
            sample.host_monotonic_ns = common::TimeUtil::MonotonicNowNs();
            if (receiver.Device()->IsConnected()) {
                sample.missed_raw = control.TryGetInt(fx10::node::kMissedTriggerCount);
                sample.temp_pcb_c = control.TryReadTemperature("ProcPCB");
                sample.temp_fpga_c = control.TryReadTemperature("FPGA");
            }
            if (start) missed_baseline = sample.missed_raw;
            sample.missed_baseline = missed_baseline;
            sample.counter_binding_verified = counter_binding_verified;
            sample.counter_regressed = sample.missed_raw && missed_baseline && *sample.missed_raw < *missed_baseline;
            sample.read_finished_monotonic_ns = common::TimeUtil::MonotonicNowNs();
            telemetry.Append(fx10::BuildTelemetryJson(sample, event));
            return sample;
        };
        auto result = nlohmann::json{{"phase", phase}, {"status", "incomplete"},
            {"session_dir", std::filesystem::relative(recorder.SessionDir(), root).generic_string()},
            {"requested_duration_s", cfg.reference.duration_s}, {"shutter_state_verified", false}};
        try {
            const bool white = std::string(phase) == "white";
            if (white) control.OpenShutter([] { return g_signal != 0; });
            else control.CloseShutter([] { return g_signal != 0; });
            fx10::ExpectedGeometry expected;
            expected.width = init.samples;
            expected.height = init.bands;
            expected.bytes_per_pixel = init.bytes_per_pixel;
            expected.payload_size = geometry.payload_size;
            expected.pixel_format = init.pixel_format;
            expected.status_line = cfg.acquisition.status_line;

            receiver.Start(recorder, expected, cfg.acquisition.frame_rate_hz, [&] {
                auto device = control.CollectDeviceMetadata();
                const auto &counter_source = device["final_readback"][fx10::node::kMissedTriggerSource];
                counter_binding_verified = counter_source["status"] == "ok" && counter_source["value"] == "MissedTrigger";
                device["reference_phase"] = phase;
                device["config_source"] = std::filesystem::relative(
                    root / "raw" / "config" / "config-fx10.yaml", recorder.SessionDir()).generic_string();
                device["created_realtime_ns"] = common::TimeUtil::RealtimeNowNs();
                device["recording_contract"] = fx10::RecordingContract(geometry.pixel_format);
                device["recording_policy"] = {{"rotation_max_lines", cfg.recording.rotation.max_lines},
                    {"rotation_max_mb", cfg.recording.rotation.max_mb}, {"flush_interval_mb", cfg.recording.flush_interval_mb},
                    {"on_gap", cfg.recording.on_gap == fx10::GapPolicy::kPadZero ? "pad_zero" : "record"}};
                device["runtime"] = receiver.RuntimeMetadata();
                device["snapshot_phase"] = "after buffer allocation, before StreamEnable/AcquisitionStart";
                device["timing"] = {{"mode", cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal
                                                            ? "external" : "freerun"},
                    {"expected_line_rate_hz", cfg.acquisition.frame_rate_hz},
                    {"sensor_trigger_enabled", cfg.sensor_trigger.enabled},
                    {"trigger_channel", cfg.sensor_trigger.trigger_channel},
                    {"observations_file", cfg.sensor_trigger.enabled ? nlohmann::json("sensor_trigger.log") : nlohmann::json(nullptr)},
                    {"association_verified", false}};
                fx10::PublishMetadata(recorder.SessionDir() / "device.json", device);
                record_telemetry("start", true);
                CheckCancelled();
            });
            if (trigger) {
                CheckCancelled();
                const double hz = cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal
                                    ? cfg.acquisition.frame_rate_hz : 0.0;
                // Protocol v2 couples each pair. Disable the other PWM group;
                // the selected output's partner necessarily pulses with it.
                std::vector<std::pair<int, double>> rates{{0, 0}, {2, 0}};
                rates.at(static_cast<std::size_t>(cfg.sensor_trigger.trigger_channel / 2)).second = hz;
                trigger->Start(recorder.SessionDir() / "sensor_trigger.log", rates);
            }
            // A steady-clock interval, not an estimated frame count. Setup and finalization are
            // outside it; preserve every in-flight frame and its native timestamps.
            const auto begin = Clock::now();
            const auto deadline = begin + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(cfg.reference.duration_s));
            result["capture_begin_monotonic_ns"] = common::TimeUtil::MonotonicNowNs();
            Marker(std::string("PHASE ") + phase);
            while (Clock::now() < deadline) {
                CheckCancelled();
                if (receiver.Failed() || recorder.Failed()) throw std::runtime_error("reference transport/writer failure");
                if (receiver.LossSeen() || recorder.LossSeen()) throw std::runtime_error("reference data loss");
                if (trigger && !trigger->Ok()) throw std::runtime_error("reference trigger log failure");
                std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(10)));
            }
            CheckCancelled();
            result["stop_requested_monotonic_ns"] = common::TimeUtil::MonotonicNowNs();
            result["capture_window_s"] = std::chrono::duration<double>(Clock::now() - begin).count();
            // With our external source, stop pulses first to avoid creating
            // missed triggers against a disarmed camera. For freerun (or an
            // independently controlled source), stop the camera immediately.
            if (trigger && cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal) trigger->Stop();
            receiver.StopAcquisition();
            // Do not move the shutter until all phase data are drained.
            cleanup.Finish("reference-duration-reached");
            CheckCancelled();
            const auto final_telemetry = record_telemetry("stop", false);
            counters.missed_trigger_delta = fx10::MissedTriggerDelta(final_telemetry).value_or(-1);
            result["missed_triggers"] = fx10::BuildTelemetryJson(final_telemetry, "stop")["missed_triggers"];
            telemetry.Close();
            if (receiver.Failed()) throw std::runtime_error("reference transport: " + receiver.ErrorMessage());
            if (recorder.Failed()) throw std::runtime_error("reference recorder: " + recorder.ErrorMessage());
            if (trigger && !trigger->Ok()) throw std::runtime_error("reference trigger stop/log integrity failure");
            if (receiver.LossSeen() || recorder.LossSeen() ||
                (counter_binding_verified && final_telemetry.counter_regressed) ||
                fx10::Classify(counters) != fx10::RunStatus::kClean) {
                throw std::runtime_error("reference recording contains lost or unconfirmed data");
            }
            if (recorder.FramesWrittenTotal() == 0) throw std::runtime_error("reference contains no frames; check configured trigger source");
            if (!receiver.DumpStreamParams(recorder.SessionDir() / "stream_stats.txt")) {
                throw std::runtime_error("cannot save reference stream statistics");
            }
            result["status"] = "completed";
        } catch (const std::exception &e) {
            cleanup.Finish("reference-failed");
            result["status"] = g_signal != 0 ? "interrupted" : "failed";
            result["error"] = e.what();
            result["frames_written"] = recorder.FramesWrittenTotal();
            fx10::PublishMetadata(recorder.SessionDir() / "reference_phase.json", result);
            throw;
        }
        result["frames_written"] = recorder.FramesWrittenTotal();
        result["bytes_written"] = counters.bytes_written;
        result["finalized_monotonic_ns"] = common::TimeUtil::MonotonicNowNs();
        result["duration_semantics"] = "configured host steady-clock window after stream/trigger startup; startup and in-flight drain frames retain their own timestamps";
        fx10::PublishMetadata(recorder.SessionDir() / "reference_phase.json", result);
        return result;
    }
}

int main(int argc, char **argv) {
    std::string config_path, output_dir, sensor_port;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::puts("Usage: fx10_reference --config <saved FX10 YAML> --out <output root> [--sensor-port <tty>]\n"
                      "Collect white (open shutter), then dark (closed), each for reference.duration_s from YAML.\n"
                      "Keeps configured device/acquisition/trigger/recording policies; writes reference_<UTC>/raw/fx10/{white,dark}/fx10_<UTC>.\n"
                      "--sensor-port: the SensorSync board (config-main.yaml 'Sensor Trigger: Port'); required when sensor_trigger.enabled.");
            return 0;
        }
        if ((arg != "--config" && arg != "--out" && arg != "--sensor-port") || i + 1 >= argc) {
            Marker("FAIL invalid arguments");
            return 2;
        }
        (arg == "--config" ? config_path : arg == "--out" ? output_dir : sensor_port) = argv[++i];
    }
    if (config_path.empty() || output_dir.empty()) {
        Marker("FAIL --config and --out are required");
        return 2;
    }
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::filesystem::path root;
    nlohmann::json summary = {{"format", "fx10-reference-v1"}, {"status", "incomplete"},
        {"phases", nlohmann::json::array()}, {"config_source", config_path},
        {"white_target", "operator positions illuminated white reference before starting"}};
    try {
        std::ifstream input(config_path);
        if (!input) throw std::runtime_error("cannot open FX10 config");
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad()) throw std::runtime_error("cannot read FX10 config");
        auto cfg = fx10::LoadAppConfigText(text); // one immutable configuration for BOTH phases
        summary["phase_duration_s"] = cfg.reference.duration_s;
        // The GUI derives its deadline and phase labels from the SAME loaded
        // configuration, not a second read of a possibly edited YAML file.
        Marker("CONFIG " + nlohmann::json{{"duration_s", cfg.reference.duration_s}}.dump());
        if (cfg.sensor_trigger.enabled && (cfg.sensor_trigger.trigger_channel < 0 || cfg.sensor_trigger.trigger_channel >= 4)) {
            throw std::runtime_error("SensorSync reference capture requires trigger_channel in [0, 3]");
        }
        if (cfg.sensor_trigger.enabled && sensor_port.empty()) {
            throw std::runtime_error("SensorSync reference capture requires --sensor-port "
                                     "(config-main.yaml 'Sensor Trigger: Port')");
        }
        root = fx10::createSessionDir(std::filesystem::absolute(output_dir), "reference",
            common::TimeUtil::CompactUtc(common::TimeUtil::RealtimeNowNs()));
        Marker("SESSION " + root.string());
        const auto config_dir = root / "raw" / "config";
        std::filesystem::create_directories(config_dir);
        std::ofstream saved(config_dir / "config-fx10.yaml");
        saved.exceptions(std::ios::failbit | std::ios::badbit);
        saved << text;
        saved.close();
        common::Logger::Config logging;
        logging.log_file = (root / "reference.log").string();
        common::Logger::Init(logging, "FX10Reference");
        summary["config_snapshot"] = "raw/config/config-fx10.yaml";
        summary["overrides"] = {{"output.max_duration_s", cfg.reference.duration_s}, {"output.max_frames", 0}};
        fx10::PublishMetadata(root / "reference.request.json", summary);
        cfg.recording.max_duration_s = cfg.reference.duration_s;
        cfg.recording.max_frames = 0;
        CheckCancelled();
        common::Ebus::BootstrapEnv();
        fx10::Counters counters;
        fx10::StreamReceiver receiver(cfg.network, counters);
        receiver.Connect(cfg.device);
        if (!fx10::PrepareFactoryDefaults(receiver, [] { return g_signal != 0; })) {
            throw std::runtime_error("reference factory preparation interrupted");
        }
        receiver.OpenStream();
        fx10::CameraControl control(*receiver.Device());
        control.ApplyAcquisitionConfig(cfg.acquisition, cfg.features.raw);
        for (const char *phase : {"white", "dark"}) {
            summary["phases"].push_back(CapturePhase(phase, cfg, sensor_port, root, receiver, control, counters));
        }
        receiver.Disconnect(); // dark phase ends closed; the next normal start explicitly opens again
        CheckCancelled();
        summary["status"] = "completed";
        fx10::PublishMetadata(root / "reference.json", summary);
        Marker("OK " + root.string());
        return 0;
    } catch (const std::exception &e) {
        summary["status"] = g_signal != 0 ? "interrupted" : "failed";
        summary["error"] = e.what();
        if (!root.empty()) {
            try { fx10::PublishMetadata(root / "reference.json", summary); } catch (...) {}
        }
        Marker("FAIL " + std::string(e.what()));
        return g_signal != 0 ? 130 : 3;
    }
}
