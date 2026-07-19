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
// Exit codes: CaptureRunner's (0/1 ok, 2 config, 3 discovery/connect/apply,
// 5 stream, 6 recorder/IO, 130 interrupted) plus 7 = clean exit but no frame
// on disk (max_duration_s safety net fired before the first frame).

#include "capture_runner.hpp"
#include "core/config.hpp"
#include "core/format.hpp"
#include "core/logger.hpp"
#include "core/signal_stop.hpp"
#include "ebus/env_bootstrap.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace {

void print_usage(const char* argv0) {
    std::printf(
        "Usage: %s --config <snapshot.json> --out <dir> [overrides]\n"
        "\n"
        "Grabs exactly one frame from one camera and writes a jai-raw-seg session\n"
        "into <dir> (frame at <dir>/<camera_id>/seg_00001.raw).\n"
        "\n"
        "Required:\n"
        "  --config <path>    strict JSONC config (see config/config-snapshot.json)\n"
        "  --out <dir>        session directory, used verbatim\n"
        "Overrides (applied to cameras[0] after the config is loaded):\n"
        "  --ip <addr>        selector = {by: ip, value: <addr>}\n"
        "  --mac <addr>       selector = {by: mac, value: <addr>}\n"
        "  --exposure-us <n>  convenience.exposure_us (microseconds, >= 0)\n"
        "  --gain <db>        convenience.gain\n"
        "  -h, --help         show this help and exit\n"
        "\n"
        "Last stdout line: \"SNAPSHOT: OK <camera_dir>\" or \"SNAPSHOT: FAIL <code> <reason>\".\n",
        argv0);
}

// Single exit funnel: every failure path emits the parseable marker.
int fail(int code, const std::string& reason) {
    std::printf("SNAPSHOT: FAIL %d %s\n", code, reason.c_str());
    std::fflush(stdout);
    return code;
}

bool parse_double(const char* s, double& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // Must run before the first eBUS SDK call (GenICam environment).
    jai::ebus::bootstrap_env();

    std::string config_path;
    std::string out_dir;
    std::string ip;
    std::string mac;
    std::optional<double> exposure_us;
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
        const char* val = argv[++i];
        if (arg == "--config") {
            config_path = val;
        } else if (arg == "--out") {
            out_dir = val;
        } else if (arg == "--ip") {
            ip = val;
        } else if (arg == "--mac") {
            mac = val;
        } else if (arg == "--exposure-us") {
            double v = 0;
            if (!parse_double(val, v) || v < 0) {
                return fail(2, std::string("bad arguments: invalid --exposure-us \"") + val + "\"");
            }
            exposure_us = v;
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
    } catch (const jai::ConfigError& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return fail(2, std::string("config-error: ") + e.what());
    }

    // CLI overrides on the first camera (load_config guarantees cameras is
    // non-empty); any further cameras are disabled — a snapshot is one frame
    // from one camera.
    jai::CameraConfig& cam = cfg.cameras[0];
    cam.enabled = true;
    for (size_t i = 1; i < cfg.cameras.size(); ++i) {
        cfg.cameras[i].enabled = false;
    }
    if (!ip.empty()) {
        cam.selector.by = "ip";
        cam.selector.value = ip;
    }
    if (!mac.empty()) {
        cam.selector.by = "mac";
        cam.selector.value = mac;
    }
    if (exposure_us) {
        cam.convenience.exposure_us = *exposure_us;
    }
    if (gain) {
        cam.convenience.gain = *gain;
    }

    // Snapshot invariants — enforced even if the config file was edited:
    // exactly one frame, bounded wall time, no PTP (free-running device
    // timestamps are fine for a preview).
    cfg.acquisition.max_frames = 1;
    if (cfg.acquisition.max_duration_s <= 0.0 || cfg.acquisition.max_duration_s > 15.0) {
        cfg.acquisition.max_duration_s = 15.0;
    }
    cfg.ptp.enabled = false;
    cam.ptp.enabled = false; // per-camera copy was deep-merged before the override

    // Mirror the effective values into cfg.raw so the session.json embedded
    // config matches what actually ran.
    try {
        cfg.raw["acquisition"]["max_frames"] = cfg.acquisition.max_frames;
        cfg.raw["acquisition"]["max_duration_s"] = cfg.acquisition.max_duration_s;
        cfg.raw["ptp"]["enabled"] = false;
        auto& raw_cam = cfg.raw["cameras"][0];
        raw_cam["selector"] = {{"by", cam.selector.by}, {"value", cam.selector.value}};
        if (cam.convenience.exposure_us) {
            raw_cam["convenience"]["exposure_us"] = *cam.convenience.exposure_us;
        }
        if (cam.convenience.gain) {
            raw_cam["convenience"]["gain"] = *cam.convenience.gain;
        }
    } catch (const nlohmann::json::exception&) {
        // Provenance only — never fail the capture over it.
    }

    const std::string camera_dir = out_dir + "/" + cam.id; // capture before move
    // A fresh segment is exactly align_up(file header, record_align) bytes;
    // any recorded frame adds at least a frame header beyond that.
    const std::uintmax_t empty_segment_size =
        jai::format::align_up(jai::format::kFileHeaderSize, cam.recording.record_align);

    jai::logger().set_level(cfg.logging.level);
    jai::StopController stop;
    jai::install_signal_handlers(&stop); // GUI cancel (SIGTERM) drains gracefully

    jai::CaptureRunner runner(std::move(cfg), &stop);
    if (runner.init(out_dir, /*validate_only=*/false)) {
        runner.run_until_stop(); // exits on LimitReached (frame 1) / signal / error
    }
    const int code = runner.shutdown();

    // A signal during the run loop drains cleanly and yields 0/1 — report it
    // as an interruption, never as success.
    if (stop.reason() == jai::StopReason::Signal) {
        return fail(130, "interrupted");
    }
    if (code != 0 && code != 1) {
        return fail(code, jai::stop_reason_name(stop.reason()));
    }

    // code 0/1: the max_duration_s safety net can stop a session with zero
    // frames and still report "clean" — verify a frame actually hit the disk.
    // (Recorder segment numbering starts at 1.)
    std::error_code ec;
    const auto size = std::filesystem::file_size(camera_dir + "/seg_00001.raw", ec);
    if (ec || size <= empty_segment_size) {
        return fail(7, "no-frame (trigger or timeout? see " + out_dir + "/events.jsonl)");
    }

    std::printf("SNAPSHOT: OK %s\n", camera_dir.c_str());
    std::fflush(stdout);
    return 0;
}
