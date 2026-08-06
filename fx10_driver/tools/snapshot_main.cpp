// fx10_snapshot: one-shot ENVI cube grab for the web GUI preview / exposure
// tuning loop, plus eBUS device discovery (--list). Reuses the fx10 pipeline
// with snapshot invariants forced (freerun, one bounded segment, tool-side
// frame counting), so the cube lands as a normal ENVI session at
// <out>/snap_<utc>/segment_0001.{bil,hdr}, decodable by the GUI.
//
// Machine-readable contracts for the GUI (like jai_snapshot, this tool never
// calls Common::Logger::init — fx10 internals land on spdlog's implicit stdout
// logger, so the GUI matches the LAST line of the shape below):
//   --list:   single-line JSON {"devices":[{display_id,connection_id,ip,mac}]}
//             on exit 0, or {"error":"..."} on exit 1
//   snapshot: SNAPSHOT: OK <session_dir>
//             SNAPSHOT: FAIL <code> <reason>
// Exit codes: 0 ok, 2 args/config/--out unusable, 3 connect/control, 5 stream
// or recorder runtime failure, 7 no frames on disk (no finalized .hdr),
// 130 interrupted.

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include "../include/accounting.hpp"
#include "../include/app_config.hpp"
#include "../include/envi_recorder.hpp"
#include "../include/pixel_format.hpp"
#include "logger.h"
#include "../include/wavelengths.hpp"
#include "ebus/camera_control.hpp"
#include "ebus/ebus_env.hpp"
#include "ebus/stream_receiver.hpp"


namespace {
    volatile std::sig_atomic_t g_signal = 0;

    void onSignal(int sig) {
        g_signal = sig;
    }

    void printUsage(const char *argv0) {
        std::printf(
            "Usage:\n"
            "  %s --list [--timeout-ms <n>]\n"
            "  %s --config <config.yaml> --out <dir> [overrides]\n"
            "\n"
            "--list prints one JSON line: {\"devices\":[{display_id,connection_id,ip,mac,configuration_valid}]}\n"
            "(exit 0) or {\"error\":\"...\"} (exit 1). Default timeout: 1500 ms.\n"
            "\n"
            "Snapshot mode grabs <frames> freerun lines from one camera and writes an\n"
            "ENVI session into <dir> (segment at <dir>/snap_<utc>/segment_0001.{bil,hdr}).\n"
            "\n"
            "Required:\n"
            "  --config <path>    YAML config (see config/config-fx10-snapshot.yaml)\n"
            "  --out <dir>        recording output_dir, used verbatim\n"
            "Overrides (applied after the config is loaded):\n"
            "  --ip <addr>        device.ip (clears device.id/mac so the IP wins)\n"
            "  --mac <addr>       device.mac (clears device.id so the MAC wins)\n"
            "  --frames <k>       lines to capture (default 64)\n"
            "  --exposure-ms <e>  acquisition.exposure_ms\n"
            "  --fps <f>          freerun frame rate (default 50; capped at 1000/exposure_ms)\n"
            "  -h, --help         show this help and exit\n"
            "\n"
            "Last stdout line: \"SNAPSHOT: OK <session_dir>\" or \"SNAPSHOT: FAIL <code> <reason>\".\n",
            argv0, argv0);
    }

    // Single exit funnel of snapshot mode: every failure path emits the marker.
    int fail(int code, const std::string &reason) {
        std::printf("SNAPSHOT: FAIL %d %s\n", code, reason.c_str());
        std::fflush(stdout);
        return code;
    }

    bool parseDouble(const char *s, double &out) {
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

    bool parseU64(const char *s, std::uint64_t &out) {
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

    int failList(const std::string &reason) {
        std::printf("%s\n", nlohmann::json{{"error", reason}}.dump().c_str());
        std::fflush(stdout);
        return 1;
    }

    int runList(int argc, char **argv) {
        std::uint64_t timeout_ms = 1500;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--list") {
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                printUsage(argv[0]);
                return 0;
            }
            if (arg == "--timeout-ms") {
                if (i + 1 >= argc || !parseU64(argv[++i], timeout_ms) || timeout_ms == 0) {
                    return failList("bad arguments: --timeout-ms requires a positive integer");
                }
            } else {
                return failList("bad arguments: unknown option " + arg + " in --list mode");
            }
        }

        // stdout stays a single JSON line: discoverDevices itself never logs, and
        // a standalone tool runs on spdlog's implicit logger (info+) anyway; the
        // GUI additionally parses only the last JSON-looking line.
        nlohmann::json doc;
        auto &arr = doc["devices"] = nlohmann::json::array();
        try {
            const auto devices = fx10::discoverDevices(std::chrono::milliseconds(timeout_ms));
            for (const auto &d: devices) {
                arr.push_back({
                    {"display_id", d.display_id},
                    {"connection_id", d.connection_id},
                    {"ip", d.ip},
                    {"mac", d.mac},
                    {"configuration_valid", d.configuration_valid}
                });
            }
        } catch (const std::exception &e) {
            return failList(e.what());
        }
        std::printf("%s\n", doc.dump().c_str());
        std::fflush(stdout);
        return 0;
    }
} // namespace


int main(int argc, char **argv) {
    fx10::bootstrapGenicamEnv(); // must precede the first eBUS SDK call (either mode)

    // Mode dispatch first: --list has a pure-JSON stdout contract.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--list") {
            return runList(argc, argv);
        }
    }

    std::string config_path;
    std::string out_dir;
    std::string ip;
    std::string mac;
    std::uint64_t frames = 64;
    std::optional<double> exposure_ms;
    double fps = 50.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
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
        } else if (arg == "--frames") {
            if (!parseU64(val, frames) || frames == 0) {
                return fail(2, std::string("bad arguments: invalid --frames \"") + val + "\"");
            }
        } else if (arg == "--exposure-ms") {
            double v = 0;
            if (!parseDouble(val, v) || v <= 0) {
                return fail(2, std::string("bad arguments: invalid --exposure-ms \"") + val + "\"");
            }
            exposure_ms = v;
        } else if (arg == "--fps") {
            double v = 0;
            if (!parseDouble(val, v) || v <= 0) {
                return fail(2, std::string("bad arguments: invalid --fps \"") + val + "\"");
            }
            fps = v;
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

    fx10::Config cfg;
    try {
        cfg = fx10::Config::loadFromFile(config_path);
    } catch (const fx10::ConfigError &e) {
        return fail(2, std::string("config-error: ") + e.what());
    }

    // Snapshot invariants — enforced even if the config file was edited: freerun
    // (a line-scan camera without external trigger pulses never delivers a
    // frame), one bounded segment under --out, gaps recorded (not padded), and
    // the frame budget counted by THIS tool, not by the recorder.
    cfg.acquisition.trigger.mode = fx10::TriggerMode::kFreerun;
    if (exposure_ms) {
        cfg.acquisition.exposure_ms = *exposure_ms;
    }
    // The freerun period must fit inside the exposure or the camera rejects
    // the AcquisitionFrameRate write outright (GENERIC_ERROR on the FX10e),
    // so cap the rate at 1000/exposure_ms.
    cfg.acquisition.frame_rate_hz = std::min(fps, 1000.0 / cfg.acquisition.exposure_ms);
    if (!ip.empty()) {
        cfg.device.ip = ip;
        cfg.device.id.clear(); // id/mac win over ip in connect(): clear them so --ip is authoritative
        cfg.device.mac.clear();
        cfg.device.force_ip.enabled = false; // force_ip requires a mac
    }
    if (!mac.empty()) {
        cfg.device.mac = mac;
        cfg.device.id.clear(); // id wins over mac in connect(): clear it so --mac is authoritative
    }
    cfg.recording.output_dir = out_dir;
    cfg.recording.base_name = "snap";
    cfg.recording.rotation.max_lines = frames + 16; // headroom so the run never rotates
    cfg.recording.rotation.max_megabytes = 0;
    cfg.recording.on_gap = fx10::GapPolicy::kRecord;
    cfg.recording.max_frames = 0;
    cfg.recording.max_duration_s = 0.0;
    cfg.logging.stats_interval_s = 0.0;

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        return fail(2, "cannot create --out directory: " + ec.message());
    }

    // GUI cancel (SIGTERM) / Ctrl+C: the eBUS calls block without a cancellation
    // API, so the flag is polled between bring-up steps and in the capture loop.
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    fx10::Counters counters;
    fx10::StreamReceiver receiver(cfg.network, counters); // dtor stops/disconnects on every path

    try {
        receiver.connect(cfg.device);
    } catch (const std::exception &e) {
        return fail(3, std::string("connect: ") + e.what());
    }
    if (g_signal != 0) {
        return fail(130, "interrupted");
    }
    try {
        receiver.openStream();
    } catch (const std::exception &e) {
        return fail(5, std::string("stream-open: ") + e.what());
    }

    std::unique_ptr<fx10::CameraControl> control;
    fx10::CameraControl::Geometry geometry;
    try {
        control = std::make_unique<fx10::CameraControl>(*receiver.device(), cfg.features);
        control->applyAcquisitionConfig(cfg.acquisition);
        geometry = control->readGeometry();
    } catch (const std::exception &e) {
        return fail(3, std::string("control: ") + e.what());
    }
    if (g_signal != 0) {
        return fail(130, "interrupted");
    }

    // Storage bpp after the receiver's unpack step (packed formats arrive as
    // canonical uint16); config validation guarantees the lookup succeeds
    const auto *pf = fx10::pixelFormatInfo(cfg.acquisition.pixel_format);
    const std::uint32_t bytes_per_pixel = pf != nullptr ? pf->storage_bpp : 2;
    fx10::RecorderInit init;
    init.samples = static_cast<std::uint32_t>(geometry.width);
    init.bands = static_cast<std::uint32_t>(geometry.height);
    init.bytes_per_pixel = bytes_per_pixel;
    init.data_type = bytes_per_pixel == 1 ? fx10::EnviDataType::kUint8 : fx10::EnviDataType::kUint16;
    try {
        init.wavelengths = fx10::resolveWavelengths(cfg.recording.wavelengths, static_cast<int>(geometry.height));
    } catch (const fx10::ConfigError &e) {
        return fail(2, std::string("wavelengths: ") + e.what());
    }
    init.description = "FX10 snapshot\npixel format: " + geometry.pixel_format +
                       "  exposure_ms: " + std::to_string(cfg.acquisition.exposure_ms) + "  freerun " +
                       std::to_string(cfg.acquisition.frame_rate_hz) + " Hz";

    fx10::EnviRecorder recorder(cfg.recording, counters);
    try {
        recorder.start(init);
    } catch (const fx10::RecorderError &e) {
        return fail(2, std::string("recorder-start: ") + e.what());
    }

    fx10::ExpectedGeometry expected;
    expected.width = static_cast<std::uint32_t>(geometry.width);
    expected.height = static_cast<std::uint32_t>(geometry.height);
    expected.bytes_per_pixel = bytes_per_pixel;
    expected.payload_size = geometry.payload_size;
    expected.pixel_format = cfg.acquisition.pixel_format;
    expected.status_line = cfg.acquisition.status_line;

    try {
        receiver.start(recorder, expected, cfg.acquisition.frame_rate_hz, cfg.watchdog, cfg.resolvedNoFrameAbortS());
    } catch (const fx10::TransportError &e) {
        recorder.stop("start-failed");
        return fail(5, std::string("stream-start: ") + e.what());
    }

    // Hard wall-clock budget: 3x the nominal capture time plus bring-up slack.
    // The freerun watchdog (no_frame_abort ~10 s) usually fires first when the
    // camera streams nothing at all.
    const double budget_s = static_cast<double>(frames) / cfg.acquisition.frame_rate_hz * 3.0 + 5.0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(budget_s));
    bool timed_out = false;
    while (g_signal == 0) {
        if (receiver.framesDelivered() >= frames) {
            break;
        }
        if (receiver.failed() || recorder.failed()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    receiver.stop();
    recorder.stop("snapshot");
    const std::string session_dir = recorder.sessionDir().string();
    receiver.disconnect();

    // A signal during the capture loop drains cleanly — report it as an
    // interruption, never as success.
    if (g_signal != 0) {
        return fail(130, "interrupted");
    }
    if (receiver.failed()) {
        // Watchdog abort in freerun == the camera delivered nothing: no-frames.
        const bool starved = receiver.fatalKind() == fx10::StreamReceiver::FatalKind::kWatchdog;
        return fail(starved ? 7 : 5, "transport: " + receiver.errorMessage());
    }
    if (recorder.failed()) {
        return fail(5, "recorder: " + recorder.errorMessage());
    }

    // The recorder silently drops 0-line segments, so `.hdr` present <=> at
    // least one line finalized on disk. This also catches a hard timeout at
    // zero frames that the watchdog did not classify.
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
        return fail(7, std::string("no-frames") + (timed_out ? " (hard timeout)" : "") + " — see " + session_dir);
    }

    std::printf("SNAPSHOT: OK %s\n", session_dir.c_str());
    std::fflush(stdout);
    return 0;
}
