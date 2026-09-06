// jai_snapshot: one-shot GigE frame grab for the web GUI preview / exposure
// tuning loop. Reuses the full CaptureRunner pipeline with snapshot
// invariants forced (max_frames=1, ptp off), so the frame lands as a normal
// jai-raw-seg session at <out>/<camera_id>/seg_00001.raw, decodable by
// scripts/unpack_raw.py.
//
// Machine-readable contract for the GUI (stdout also carries LOG_* noise, so
// the GUI matches the last line prefixed "SNAPSHOT: "):
//   SNAPSHOT: OK <camera_dir>
//   SNAPSHOT: FAIL <code> <reason>
// Exit codes (assigned HERE, fx10_snapshot-style — the driver core reports
// concrete errors, not codes): 0 ok, 2 args/config, 3 startup, 5 capture
// error, 7 = no frame on disk (nothing streamed: the 10 s no-frame bound below
// fired, or the max_duration_s safety net expired first), 130 interrupted.

#include "capture_runner.h"
#include "app_config.h"
#include "format.h"
#include "logger.h"
#include "signal_stop.h"
#include "string_util.h"
#include "ebus/env_bootstrap.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace {
    void PrintUsage(const char *argv0) {
        std::printf(
            "Usage: %s --config <snapshot.yaml> --out <dir> [overrides]\n"
            "\n"
            "Grabs exactly one frame from one camera and writes a jai-raw-seg session\n"
            "into <dir> (frame at <dir>/<camera_id>/seg_00001.raw).\n"
            "\n"
            "Required:\n"
            "  --config <path>    YAML config (see config/config-gox-snapshot.yaml)\n"
            "  --out <dir>        session directory, used verbatim\n"
            "Overrides (applied to cameras[0] after the config is loaded):\n"
            "  --ip <addr>        device.ip (clears device.mac so the IP wins)\n"
            "  --mac <addr>       device.mac (discovery match; wins over ip)\n"
            "  --exposure-ms <n>  acquisition exposure (milliseconds, >= 0)\n"
            "  --gain <x>         acquisition.gain (magnification, 1.0-126.0; NOT dB)\n"
            "  -h, --help         show this help and exit\n"
            "\n"
            "Last stdout line: \"SNAPSHOT: OK <camera_dir>\" or \"SNAPSHOT: FAIL <code> <reason>\".\n",
            argv0);
    }

    // Single exit funnel: every failure path emits the parseable marker. The
    // reason is flattened because the GUI matches the LAST stdout line, and a
    // driver error can be multi-line (discovery lists every device it saw).
    int Fail(int code, const std::string &reason) {
        std::printf("SNAPSHOT: FAIL %d %s\n", code, common::StringUtil::OneLine(reason).c_str());
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
} // namespace

int main(int argc, char **argv) {
    // Must run before the first eBUS SDK call (GenICam environment).
    common::Ebus::BootstrapEnv();

    std::string config_path;
    std::string out_dir;
    std::string ip;
    std::string mac;
    std::optional<double> exposure_ms;
    std::optional<double> gain;

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
        } else if (arg == "--exposure-ms") {
            double v = 0;
            if (!ParseDouble(val, v) || v < 0) {
                return Fail(2, std::string("bad arguments: invalid --exposure-ms \"") + val + "\"");
            }
            exposure_ms = v;
        } else if (arg == "--gain") {
            double v = 0;
            if (!ParseDouble(val, v)) {
                return Fail(2, std::string("bad arguments: invalid --gain \"") + val + "\"");
            }
            gain = v;
        } else {
            return Fail(2, "bad arguments: unknown option " + arg + " (see --help)");
        }
    }
    if (config_path.empty() || out_dir.empty()) {
        return Fail(2, "--config and --out are required (see --help)");
    }
    if (!ip.empty() && !mac.empty()) {
        return Fail(2, "--ip and --mac are mutually exclusive");
    }

    gox::AppConfig cfg;
    try {
        cfg = gox::LoadAppConfig(config_path);
    } catch (const gox::ConfigError &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return Fail(2, std::string("config-error: ") + e.what());
    }

    // CLI overrides on the first camera (LoadAppConfig guarantees cameras is
    // non-empty); any further cameras are disabled — a snapshot is one frame
    // from one camera.
    gox::CameraConfig &cam = cfg.cameras[0];
    cam.enabled = true;
    for (size_t i = 1; i < cfg.cameras.size(); ++i) {
        cfg.cameras[i].enabled = false;
    }
    if (!ip.empty()) {
        cam.device.ip = ip;
        cam.device.mac.clear(); // mac wins over ip in Connect(): clear it so --ip is authoritative
    }
    if (!mac.empty()) {
        cam.device.mac = mac;
    }
    if (exposure_ms) {
        // In freerun the exposure cannot exceed the frame period; the camera
        // would clamp it, and a clamped strict write fails the bring-up.
        if (cam.acquisition.frame_rate_hz && *exposure_ms >= 1000.0 / *cam.acquisition.frame_rate_hz) {
            return Fail(2, "--exposure-ms " + std::to_string(*exposure_ms) +
                           " does not fit in the configured frame period of " +
                           std::to_string(1000.0 / *cam.acquisition.frame_rate_hz) +
                           " ms; lower the exposure or acquisition.frame_rate_hz in the config");
        }
        cam.acquisition.exposure_ms = exposure_ms;
    }
    if (gain) {
        cam.acquisition.gain = *gain;
    }

    // Snapshot invariants — enforced even if the config file was edited:
    // exactly one frame, bounded wall time, no PTP (free-running device
    // timestamps are fine for a preview).
    cfg.output.max_frames = 1;
    if (cfg.output.max_duration_s <= 0.0 || cfg.output.max_duration_s > 15.0) {
        cfg.output.max_duration_s = 15.0;
    }
    cfg.ptp.enabled = false;
    cam.acquisition.trigger.mode = gox::TriggerMode::kFreerun; // a preview must not wait for trigger pulses
    cfg.stats_interval_s = 0; // a single frame needs no periodic stats line

    const std::string camera_dir = out_dir + "/" + cam.id; // capture before move
    // A fresh segment is exactly AlignUp(file header, record_align) bytes;
    // any recorded frame adds at least a frame header beyond that.
    const std::uintmax_t empty_segment_size =
            gox::format::AlignUp(gox::format::kFileHeaderSize, cfg.output.record_align);

    gox::StopController stop;
    gox::InstallSignalHandlers(&stop); // GUI cancel (SIGTERM) drains gracefully

    gox::CaptureRunner runner(std::move(cfg), &stop);
    const bool started = runner.Init(out_dir);
    // Fast fail on a camera that streams nothing. This tool does not run under
    // Main, so config-main.yaml's Guards never apply to it and it keeps a bound
    // of its own; without it the only backstop is max_duration_s (15 s) and the
    // reason string degrades to a bare timeout.
    constexpr std::uint64_t kNoFrameTimeoutUs = 10 * 1000 * 1000;
    bool starved = false;
    if (started) {
        // exits on LimitReached (frame 1) / signal / error / starvation
        runner.MonitorLoop([&] {
            const auto silent_us = runner.MicrosSinceLastData();
            if (silent_us && *silent_us >= kNoFrameTimeoutUs) {
                starved = true;
                return true;
            }
            return false;
        });
    }
    const bool clean = runner.Shutdown();

    // A signal during the run loop drains cleanly — report it as an
    // interruption, never as success.
    if (stop.Reason() == gox::StopReason::kSignal) {
        return Fail(130, "interrupted");
    }
    if (!started) {
        return Fail(3, runner.LastError());
    }

    // Did a frame actually reach the disk? This is decided BEFORE the generic
    // capture-error path: the commonest failure is that the camera streams
    // nothing, and code 7 is the contract the GUI reads as "no frame".
    // Recorder numbering starts at 1.
    std::error_code ec;
    const auto size = std::filesystem::file_size(camera_dir + "/seg_00001.raw", ec);
    if (ec || size <= empty_segment_size) {
        std::string reason = starved
                                 ? "no-frame (the camera delivered nothing in 10 s)"
                                 : "no-frame (trigger or timeout? see the log)";
        if (!runner.LastError().empty()) {
            reason += ": " + runner.LastError();
        }
        return Fail(7, reason);
    }

    // A frame is on disk but the session still ended badly: a genuine capture
    // Error (drops cannot happen with max_frames=1 + queue_on_full=block).
    if (!clean && stop.Reason() == gox::StopReason::kError) {
        return Fail(5, runner.LastError());
    }

    std::printf("SNAPSHOT: OK %s\n", camera_dir.c_str());
    std::fflush(stdout);
    return 0;
}
