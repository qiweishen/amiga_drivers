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
// error, 7 = clean exit but no frame on disk (max_duration_s safety net
// fired before the first frame), 130 interrupted.

#include "capture_runner.hpp"
#include "app_config.hpp"
#include "format.hpp"
#include "logger.h"
#include "signal_stop.hpp"
#include "ebus/env_bootstrap.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace {
    void print_usage(const char *argv0) {
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
            "  --gain <db>        acquisition.gain\n"
            "  -h, --help         show this help and exit\n"
            "\n"
            "Last stdout line: \"SNAPSHOT: OK <camera_dir>\" or \"SNAPSHOT: FAIL <code> <reason>\".\n",
            argv0);
    }

    // Single exit funnel: every failure path emits the parseable marker.
    int fail(int code, const std::string &reason) {
        std::printf("SNAPSHOT: FAIL %d %s\n", code, reason.c_str());
        std::fflush(stdout);
        return code;
    }

    bool parse_double(const char *s, double &out) {
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
    jai::ebus::bootstrap_env();

    std::string config_path;
    std::string out_dir;
    std::string ip;
    std::string mac;
    std::optional<double> exposure_ms;
    std::optional<double> gain;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            return fail(2, "bad arguments: " + arg + " requires a value (see --help)");
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
            if (!parse_double(val, v) || v < 0) {
                return fail(2, std::string("bad arguments: invalid --exposure-ms \"") + val + "\"");
            }
            exposure_ms = v;
        } else if (arg == "--gain") {
            double v = 0;
            if (!parse_double(val, v)) {
                return fail(2, std::string("bad arguments: invalid --gain \"") + val + "\"");
            }
            gain = v;
        } else {
            return fail(2, "bad arguments: unknown option " + arg + " (see --help)");
        }
    }
    if (config_path.empty() || out_dir.empty()) {
        return fail(2, "--config and --out are required (see --help)");
    }
    if (!ip.empty() && !mac.empty()) {
        return fail(2, "--ip and --mac are mutually exclusive");
    }

    jai::AppConfig cfg;
    try {
        cfg = jai::load_config(config_path);
    } catch (const jai::ConfigError &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return fail(2, std::string("config-error: ") + e.what());
    }

    // CLI overrides on the first camera (load_config guarantees cameras is
    // non-empty); any further cameras are disabled — a snapshot is one frame
    // from one camera.
    jai::CameraConfig &cam = cfg.cameras[0];
    cam.enabled = true;
    for (size_t i = 1; i < cfg.cameras.size(); ++i) {
        cfg.cameras[i].enabled = false;
    }
    if (!ip.empty()) {
        cam.device.ip = ip;
        cam.device.mac.clear(); // mac wins over ip in connect(): clear it so --ip is authoritative
        cam.device.force_ip.enabled = false; // force_ip requires a mac
    }
    if (!mac.empty()) {
        cam.device.mac = mac;
    }
    if (exposure_ms) {
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
    cam.acquisition.trigger.mode = "freerun"; // a preview must not wait for trigger pulses
    cfg.stats_interval_s = 0; // a single frame needs no periodic stats line

    const std::string camera_dir = out_dir + "/" + cam.id; // capture before move
    // A fresh segment is exactly align_up(file header, record_align) bytes;
    // any recorded frame adds at least a frame header beyond that.
    const std::uintmax_t empty_segment_size =
            jai::format::align_up(jai::format::kFileHeaderSize, cfg.output.record_align);

    jai::StopController stop;
    jai::install_signal_handlers(&stop); // GUI cancel (SIGTERM) drains gracefully

    jai::CaptureRunner runner(std::move(cfg), &stop);
    const bool started = runner.init(out_dir);
    if (started) {
        runner.monitor_loop(); // exits on LimitReached (frame 1) / signal / error
    }
    const bool clean = runner.shutdown();

    // A signal during the run loop drains cleanly — report it as an
    // interruption, never as success.
    if (stop.reason() == jai::StopReason::Signal) {
        return fail(130, "interrupted");
    }
    if (!started) {
        return fail(3, runner.last_error());
    }
    // Frame drops cannot happen with max_frames=1 + queue_on_full=block, so an
    // unclean single-shot session is a genuine capture error.
    if (!clean && stop.reason() == jai::StopReason::Error) {
        return fail(5, runner.last_error());
    }

    // code 0/1: the max_duration_s safety net can stop a session with zero
    // frames and still report "clean" — verify a frame actually hit the disk.
    // (Recorder segment numbering starts at 1.)
    std::error_code ec;
    const auto size = std::filesystem::file_size(camera_dir + "/seg_00001.raw", ec);
    if (ec || size <= empty_segment_size) {
        return fail(7, "no-frame (trigger or timeout? see the log)");
    }

    std::printf("SNAPSHOT: OK %s\n", camera_dir.c_str());
    std::fflush(stdout);
    return 0;
}
