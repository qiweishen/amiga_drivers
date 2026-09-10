#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include "../include/accounting.h"
#include "../include/app_config.h"
#include "../include/envi_recorder.h"
#include "../include/pixel_format.h"
#include "logger.h"
#include "../include/wavelengths.h"
#include "ebus/camera_control.h"
#include "ebus/env_bootstrap.h" // common/ (amiga_ebus)
#include "ebus/stream_receiver.h"


namespace {
    volatile std::sig_atomic_t g_signal = 0;

    void OnSignal(int sig) {
        g_signal = sig;
    }

    void PrintUsage(const char *argv0) {
        std::printf(
            "Usage:\n"
            "  %s --config <config.yaml> --out <dir> [overrides]\n"
            "\n"
            "Grabs <frames> freerun lines from one camera and writes an\n"
            "ENVI session into <dir> (segment at <dir>/fx10_<utc>/segment_0001.{bil,hdr}).\n"
            "\n"
            "Required:\n"
            "  --config <path>    YAML config (see config/config-fx10-snapshot.yaml)\n"
            "  --out <dir>        recording output_dir, used verbatim\n"
            "Overrides (applied after the config is loaded):\n"
            "  --ip <addr>        device.ip (clears device.mac so the IP wins)\n"
            "  --mac <addr>       device.mac (wins over device.ip)\n"
            "  --frames <k>       lines to capture (default 64)\n"
            "  --exposure-ms <e>  acquisition.exposure_ms\n"
            "  --spatial-binning <n>   acquisition.spatial_binning  [1|2|4|8]\n"
            "  --spectral-binning <n>  acquisition.spectral_binning [1|2|4|8] (mroi requires 1)\n"
            "  --fps <f>          freerun frame rate (default 50; capped at 1000/exposure_ms)\n"
            "  -h, --help         show this help and exit\n"
            "\n"
            "Last stdout line: \"SNAPSHOT: OK <session_dir>\" or \"SNAPSHOT: FAIL <code> <reason>\".\n",
            argv0);
    }

    // Single exit funnel of snapshot mode: every failure path emits the marker.
    int Fail(int code, const std::string &reason) {
        std::printf("SNAPSHOT: FAIL %d %s\n", code, reason.c_str());
        std::fflush(stdout);
        return code;
    }

    bool ParseDouble(const char *s, double &out) {
        if (s == nullptr || *s == '\0') {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        const double v = std::strtod(s, &end);
        if (errno != 0 || end == s || *end != '\0') {
            return false;
        }
        out = v;
        return true;
    }

    bool ParseU64(const char *s, std::uint64_t &out) {
        if (s == nullptr || *s == '\0') {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        const unsigned long long v = std::strtoull(s, &end, 10);
        if (errno != 0 || end == s || *end != '\0') {
            return false;
        }
        out = v;
        return true;
    }

} // namespace


int main(int argc, char **argv) {
    common::Ebus::BootstrapEnv(); // must precede the first eBUS SDK call

    std::string config_path;
    std::string out_dir;
    std::string ip;
    std::string mac;
    std::uint64_t frames = 64;
    std::optional<double> exposure_ms;
    std::optional<int> spatial_binning;
    std::optional<int> spectral_binning;
    double fps = 50.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            return Fail(2, "bad arguments: " + arg + " requires a value (see --help)");
        }
        const char *val = argv[++i];
        if (arg == "--config") {
            config_path = val;
        } else if (arg == "--out") {
            out_dir = val;
        } else if (arg == "--ip") {
            ip = val;
        } else if (arg == "--mac") {
            mac = val;
        } else if (arg == "--frames") {
            if (!ParseU64(val, frames) || frames == 0) {
                return Fail(2, std::string("bad arguments: invalid --frames \"") + val + "\"");
            }
        } else if (arg == "--exposure-ms") {
            double v = 0;
            if (!ParseDouble(val, v) || v <= 0) {
                return Fail(2, std::string("bad arguments: invalid --exposure-ms \"") + val + "\"");
            }
            exposure_ms = v;
        } else if (arg == "--spatial-binning" || arg == "--spectral-binning") {
            std::uint64_t v = 0;
            if (!ParseU64(val, v) || (v != 1 && v != 2 && v != 4 && v != 8)) {
                return Fail(2, "bad arguments: " + arg + " must be 1, 2, 4, or 8 (got \"" +
                               std::string(val) + "\")");
            }
            (arg == "--spatial-binning" ? spatial_binning : spectral_binning) = static_cast<int>(v);
        } else if (arg == "--fps") {
            double v = 0;
            if (!ParseDouble(val, v) || v <= 0) {
                return Fail(2, std::string("bad arguments: invalid --fps \"") + val + "\"");
            }
            fps = v;
        } else {
            return Fail(2, "bad arguments: unknown option " + arg + " (see --help)");
        }
    }
    if (!ip.empty() && !mac.empty()) {
        return Fail(2, "--ip and --mac are mutually exclusive");
    }

    fx10::AppConfig cfg;
    try {
        cfg = fx10::LoadAppConfig(config_path);
    } catch (const fx10::ConfigError &e) {
        return Fail(2, std::string("config-error: ") + e.what());
    }

    // Snapshot invariants — enforced even if the config file was edited: freerun
    // (a line-scan camera without external trigger pulses never delivers a
    // frame), one bounded segment under --out, gaps recorded (not padded), and
    // the frame budget counted by THIS tool, not by the recorder.
    cfg.acquisition.trigger.mode = fx10::TriggerMode::kFreerun;
    if (exposure_ms) {
        cfg.acquisition.exposure_ms = *exposure_ms;
    }
    if (spatial_binning) {
        cfg.acquisition.spatial_binning = *spatial_binning;
    }
    if (spectral_binning) {
        // Config validation ran at load time, so an override cannot be allowed
        // to sneak past the FX10's own constraint.
        if (*spectral_binning != 1 && cfg.acquisition.mroi.enabled) {
            return Fail(2, "--spectral-binning must be 1 while acquisition.mroi.enabled is true "
                           "(FX10 constraint)");
        }
        cfg.acquisition.spectral_binning = *spectral_binning;
    }
    // The freerun period must fit inside the exposure or the camera rejects
    // the AcquisitionFrameRate write outright (GENERIC_ERROR on the FX10e),
    // so cap the rate at 1000/exposure_ms.
    cfg.acquisition.frame_rate_hz = std::min(fps, 1000.0 / cfg.acquisition.exposure_ms);
    if (!ip.empty()) {
        cfg.device.ip = ip;
        cfg.device.mac.clear(); // mac wins over ip in Connect(): clear it so --ip is authoritative
    }
    if (!mac.empty()) {
        cfg.device.mac = mac;
    }
    cfg.recording.output_dir = out_dir;
    cfg.recording.rotation.max_lines = frames + 16; // headroom so the run never rotates
    cfg.recording.rotation.max_mb = 0;
    cfg.recording.on_gap = fx10::GapPolicy::kRecord;
    cfg.recording.max_frames = 0;
    cfg.recording.max_duration_s = 0.0;
    cfg.logging.stats_interval_s = 0.0;

    // GUI cancel (SIGTERM) / Ctrl+C: the eBUS calls block without a cancellation
    // API, so the flag is polled between bring-up steps and in the capture loop.
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    fx10::Counters counters;
    fx10::StreamReceiver receiver(cfg.network, counters); // dtor stops/disconnects on every path

    try {
        receiver.Connect(cfg.device);
    } catch (const std::exception &e) {
        return Fail(3, std::string("connect: ") + e.what());
    }
    if (g_signal != 0) {
        return Fail(130, "interrupted");
    }
    // NOTE: this bring-up mirrors Fx10DriverApp::BringUpSession()
    // (src/fx10_driver_app.cpp) step for step. The two are deliberately NOT
    // shared in full: the snapshot's invariants differ (freerun forced, tool-side frame
    // budget, no trigger box, no reconnect loop) and every failure exit is a
    // different stdout contract. Factory preparation and OpenShutter are shared.
    //
    // Factory baseline before the stream open: the load resets GevSCPSPacketSize
    std::unique_ptr<fx10::CameraControl> control;
    try {
        control = std::make_unique<fx10::CameraControl>(*receiver.Device());
        control.reset();
        if (!fx10::PrepareFactoryDefaults(receiver, [] { return g_signal != 0; })) {
            return Fail(130, "interrupted");
        }
        control = std::make_unique<fx10::CameraControl>(*receiver.Device());
    } catch (const std::exception &e) {
        return Fail(3, std::string("control: ") + e.what());
    }
    if (g_signal != 0) {
        return Fail(130, "interrupted");
    }
    try {
        receiver.OpenStream();
    } catch (const std::exception &e) {
        return Fail(5, std::string("stream-open: ") + e.what());
    }

    fx10::CameraControl::Geometry geometry;
    try {
        control->ApplyAcquisitionConfig(cfg.acquisition, cfg.features.raw);
        geometry = control->ReadGeometry();
    } catch (const std::exception &e) {
        return Fail(3, std::string("control: ") + e.what());
    }
    if (g_signal != 0) {
        return Fail(130, "interrupted");
    }

    // Storage bpp after the receiver's unpack step (packed formats arrive as
    // canonical uint16); config validation guarantees the lookup succeeds
    const auto *pf = fx10::GetPixelFormatInfo(cfg.acquisition.pixel_format);
    const std::uint32_t bytes_per_pixel = pf != nullptr ? pf->storage_bpp : 2;
    fx10::RecorderInit init;
    init.samples = static_cast<std::uint32_t>(geometry.width);
    init.bands = static_cast<std::uint32_t>(geometry.height);
    init.bytes_per_pixel = bytes_per_pixel;
    init.pixel_format = geometry.pixel_format;
    init.expected_frame_rate_hz = cfg.acquisition.frame_rate_hz;
    init.data_type = bytes_per_pixel == 1 ? fx10::EnviDataType::kUint8 : fx10::EnviDataType::kUint16;
    try {
        const std::string serial = cfg.recording.wavelengths.source == fx10::WavelengthSource::kNone
                                       ? "" : control->GetString("DeviceSerialNumber");
        init.wavelengths = fx10::ResolveForGeometry(cfg.recording.wavelengths, serial, geometry.width, geometry.height,
                                                   geometry.offset_x, geometry.offset_y, cfg.acquisition.status_line);
    } catch (const std::exception &e) {
        return Fail(2, std::string("wavelengths: ") + e.what());
    }
    init.description = "FX10 snapshot\npixel format: " + geometry.pixel_format +
                       "  exposure_ms: " + std::to_string(cfg.acquisition.exposure_ms) + "  freerun " +
                       std::to_string(cfg.acquisition.frame_rate_hz) + " Hz";

    fx10::EnviRecorder recorder(cfg.recording, counters);
    // Declared after recorder: joins both receiver workers before the sink can
    // be destroyed, including exceptions during partial Start or preview setup.
    struct ReceiverStopGuard {
        fx10::StreamReceiver &receiver;
        ~ReceiverStopGuard() {
            try { receiver.Stop(); } catch (...) {}
        }
    } receiver_stop_guard{receiver};
    try {
        recorder.Start(init);
    } catch (const fx10::RecorderError &e) {
        return Fail(2, std::string("recorder-start: ") + e.what());
    }

    fx10::ExpectedGeometry expected;
    expected.width = static_cast<std::uint32_t>(geometry.width);
    expected.height = static_cast<std::uint32_t>(geometry.height);
    expected.bytes_per_pixel = bytes_per_pixel;
    expected.payload_size = geometry.payload_size;
    expected.pixel_format = cfg.acquisition.pixel_format;
    expected.status_line = cfg.acquisition.status_line;

    try {
        control->OpenShutter([] { return g_signal != 0; });
        receiver.Start(recorder, expected, cfg.acquisition.frame_rate_hz, [] {
            if (g_signal != 0) throw fx10::ControlError("[eBUS] acquisition start interrupted");
        });
    } catch (const std::exception &e) {
        receiver.Stop();
        recorder.Stop("start-failed");
        return Fail(g_signal != 0 ? 130 : 5, std::string("stream-start: ") + e.what());
    }

    // Two independent bounds. The wall-clock budget covers "the capture is
    // dragging" (3x the nominal time plus bring-up slack); kNoFrameTimeoutS
    // covers "the camera is streaming nothing at all", which used to be caught
    // by the receiver watchdog. Without the second, `--frames 5000 --fps 50`
    // would make the GUI preview wait 305 s to learn the camera is dead.
    // This tool keeps its own bound on purpose: it does not run under Main,
    // so config-main.yaml's Guards never apply to it.
    constexpr double kNoFrameTimeoutS = 10.0;
    const double budget_s = static_cast<double>(frames) / cfg.acquisition.frame_rate_hz * 3.0 + 5.0;
    const auto after = [](double seconds) {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(seconds));
    };
    const auto deadline = std::chrono::steady_clock::now() + after(budget_s);
    const auto starve_deadline = std::chrono::steady_clock::now() + after(kNoFrameTimeoutS);
    bool timed_out = false;
    bool starved = false;
    while (g_signal == 0) {
        if (receiver.FramesDelivered() >= frames) {
            break;
        }
        if (receiver.Failed() || recorder.Failed()) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (receiver.FramesDelivered() == 0 && now >= starve_deadline) {
            starved = true;
            timed_out = true;
            break;
        }
        if (now >= deadline) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // No early return: the stream and the recorder are torn down below on every
    // path, so a starved capture still finalizes and reports through the same
    // no-frames branch.

    receiver.Stop();
    recorder.Stop("snapshot");
    const std::string session_dir = recorder.SessionDir().string();
    receiver.Disconnect();

    // A signal during the capture loop drains cleanly — report it as an
    // interruption, never as success.
    if (g_signal != 0) {
        return Fail(130, "interrupted");
    }
    if (receiver.Failed()) {
        return Fail(5, "transport: " + receiver.ErrorMessage());
    }
    if (recorder.Failed()) {
        return Fail(5, "recorder: " + recorder.ErrorMessage());
    }

    // The recorder silently drops 0-line segments, so `.hdr` present <=> at
    // least one line finalized on disk. This also catches the case where frames
    // arrived but none of them reached the file.
    bool have_hdr = false;
    std::error_code walk_ec;
    for (std::filesystem::directory_iterator it(session_dir, walk_ec), end; !walk_ec && it != end;
         it.increment(walk_ec)) {
        if (it->path().extension() == ".hdr") {
            have_hdr = true;
            break;
        }
    }
    if (!have_hdr) {
        const std::string why = starved
                                    ? fmt::format(" (the camera delivered nothing in {:.0f} s)", kNoFrameTimeoutS)
                                    : (timed_out ? std::string(" (hard timeout)") : std::string());
        return Fail(7, "no-frames" + why + " — see " + session_dir);
    }

    std::printf("SNAPSHOT: OK %s\n", session_dir.c_str());
    std::fflush(stdout);
    return 0;
}
