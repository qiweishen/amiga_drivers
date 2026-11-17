// GUI-compatible spectral preview command; native SDK bytes are retained in RAW.
#include "../include/app_config.h"
#include "ebus_driver/tool_support.h"
#include "freerun_config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace {
constexpr double kMaxStreamSeconds = 3600.0;
// Live stream: about this many seconds of lines per RAW segment, so the GUI can delete
// the closed segments behind the newest ones at any frame rate.
constexpr double kStreamSegmentSeconds = 0.25;

void Usage(const char *program) {
    std::printf("Usage: %s --config <snapshot.yaml> --out <dir> [overrides]\n"
        "Captures FX10 scan lines in freerun (SensorSync off) under <dir>/fx10_<UTC>.\n"
        "Overrides: --ip <address> | --mac <address>, --frames <count> (64), --exposure-ms <value>,\n"
        "  --spatial-binning <1|2|4|8>, --spectral-binning <1|2|4|8>, --fps <value> (config rate)\n"
        "Live stream: --stream-s <1..3600> keeps acquiring for up to that many seconds instead of\n"
        "  --frames, about 0.25 s of lines per RAW segment; SIGINT/SIGTERM is the normal end.\n"
        "Last stdout line: SNAPSHOT: OK <session_dir> or SNAPSHOT: FAIL <code> <reason>\n", program);
}
}

int main(int argc, char **argv) {
    using namespace ebus_driver::tool;
    std::string config_path, output, ip, mac;
    std::optional<double> exposure, fps, stream_s;
    std::optional<int> spatial, spectral;
    std::uint64_t frames = 64;
    bool frames_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { Usage(argv[0]); return 0; }
        if (i + 1 >= argc) return SnapshotFail(2, "missing value for " + arg);
        const char *value = argv[++i];
        if (arg == "--config") config_path = value;
        else if (arg == "--out") output = value;
        else if (arg == "--ip") ip = value;
        else if (arg == "--mac") mac = value;
        else if (arg == "--frames") {
            if (!ParseCount(value, frames) || frames == 0) return SnapshotFail(2, "invalid --frames");
            frames_given = true;
        } else if (arg == "--spatial-binning" || arg == "--spectral-binning") {
            std::uint64_t number = 0;
            if (!ParseCount(value, number) || (number != 1 && number != 2 && number != 4 && number != 8))
                return SnapshotFail(2, "invalid " + arg);
            (arg == "--spatial-binning" ? spatial : spectral) = static_cast<int>(number);
        } else if (arg == "--exposure-ms" || arg == "--fps") {
            double number = 0;
            if (!ParseDouble(value, number) || number <= 0.0) return SnapshotFail(2, "invalid " + arg);
            if (arg == "--fps") fps = number; else exposure = number;
        } else if (arg == "--stream-s") {
            double number = 0;
            if (!ParseDouble(value, number) || number < 1.0 || number > kMaxStreamSeconds)
                return SnapshotFail(2, "invalid --stream-s");
            stream_s = number;
        } else return SnapshotFail(2, "unknown option " + arg);
    }
    if (config_path.empty() || output.empty()) return SnapshotFail(2, "--config and --out are required");
    if (!ip.empty() && !mac.empty()) return SnapshotFail(2, "--ip and --mac are mutually exclusive");
    if (frames_given && stream_s) return SnapshotFail(2, "--frames and --stream-s are mutually exclusive");
    try {
        auto config = fx10::LoadAppConfig(config_path);
        if (!ip.empty()) { config.device.ip = ip; config.device.mac.clear(); }
        if (!mac.empty()) config.device.mac = mac;
        if (exposure) config.acquisition.exposure_ms = *exposure;
        if (spatial) config.acquisition.spatial_binning = *spatial;
        if (spectral) config.acquisition.spectral_binning = *spectral;
        if (fps) config.acquisition.frame_rate_hz = *fps;
        // Same limits as the loader, after the overrides (binning changes the rate ceiling).
        // Nothing is capped silently: the GUI passes a rate that already fits.
        fx10::CheckAcquisitionTiming(config.acquisition);
        fx10::tool::ForceFreerun(config);
        config.recording.rotation.max_mb = 0;
        if (stream_s) {
            // Live stream (Camera Tools focus assist): no frame limit. The GUI ends it with
            // SIGTERM; the duration bounds a stream whose GUI went away. The GUI reads the
            // newest lines and deletes the closed segments behind them.
            config.recording.max_frames = 0;
            config.recording.max_duration_s = *stream_s;
            config.recording.rotation.max_lines = static_cast<std::uint64_t>(
                std::max(1.0, std::ceil(config.acquisition.frame_rate_hz * kStreamSegmentSeconds)));
            return RunStream(ebus_driver::MakeFx10Camera(config, config_path, TimestampedPath(output, "fx10")));
        }
        config.recording.max_frames = frames;
        config.recording.max_duration_s = std::max(5.0, static_cast<double>(frames) /
            config.acquisition.frame_rate_hz * 3.0 + 5.0);
        if (!std::isfinite(config.recording.max_duration_s) || config.recording.max_duration_s > 604800.0)
            return SnapshotFail(2, "snapshot duration exceeds seven days");
        config.recording.rotation.max_lines = 0;
        return RunSnapshot(ebus_driver::MakeFx10Camera(config, config_path, TimestampedPath(output, "fx10")), frames);
    } catch (const std::exception &error) { return SnapshotFail(2, error.what()); }
}
