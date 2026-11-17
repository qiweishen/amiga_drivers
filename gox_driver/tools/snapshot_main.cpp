// GUI-compatible one-camera preview command using the common eBUS RAW engine.
#include "../include/app_config.h"
#include "../include/buffering.h"
#include "ebus_driver/tool_support.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

namespace {
constexpr double kMaxStreamSeconds = 3600.0;

void Usage(const char *program) {
    std::printf("Usage: %s --config <snapshot.yaml> --out <dir> [overrides]\n"
        "Captures one GOX frame in freerun (PTP and SensorSync off) under <dir>/<camera_id>.\n"
        "Overrides: --ip <address> | --mac <address>, --exposure-ms <value>, --gain <value>,\n"
        "           --frame-rate-hz <value>\n"
        "Live stream: --stream-s <1..3600> keeps acquiring for up to that many seconds instead of\n"
        "           one frame, one record per RAW segment; SIGINT/SIGTERM is the normal end.\n"
        "           The focus-assist stream is BayerRG8 (--frame-rate-hz up to 9.078)\n"
        "Last stdout line: SNAPSHOT: OK <camera_dir> or SNAPSHOT: FAIL <code> <reason>\n", program);
}
}

int main(int argc, char **argv) {
    using namespace ebus_driver::tool;
    std::string config_path, output, ip, mac;
    std::optional<double> exposure, gain, frame_rate, stream_s;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { Usage(argv[0]); return 0; }
        if (i + 1 >= argc) return SnapshotFail(2, "missing value for " + arg);
        const char *value = argv[++i];
        if (arg == "--config") config_path = value;
        else if (arg == "--out") output = value;
        else if (arg == "--ip") ip = value;
        else if (arg == "--mac") mac = value;
        else if (arg == "--exposure-ms" || arg == "--gain" || arg == "--frame-rate-hz" || arg == "--stream-s") {
            double number = 0;
            const bool bad = !ParseDouble(value, number) ||
                (arg == "--exposure-ms" ? number <= 0.0 || number > gox::kMaxExposureMs
                 : arg == "--gain" ? number < 1.0 || number > 126.0
                 : arg == "--stream-s" ? number < 1.0 || number > kMaxStreamSeconds
                 : number < 0.125 || number > gox::kMaxFocusFrameRateHz); // snapshot ceiling checked below
            if (bad) return SnapshotFail(2, "invalid value for " + arg);
            (arg == "--gain" ? gain : arg == "--exposure-ms" ? exposure
             : arg == "--stream-s" ? stream_s : frame_rate) = number;
        } else return SnapshotFail(2, "unknown option " + arg);
    }
    if (config_path.empty() || output.empty()) return SnapshotFail(2, "--config and --out are required");
    if (!ip.empty() && !mac.empty()) return SnapshotFail(2, "--ip and --mac are mutually exclusive");
    if (frame_rate && !stream_s && *frame_rate > gox::kMaxFrameRateHz)
        return SnapshotFail(2, "invalid value for --frame-rate-hz");
    try {
        auto config = gox::LoadAppConfig(config_path);
        auto &camera = config.cameras.at(0);
        camera.enabled = true;
        for (std::size_t i = 1; i < config.cameras.size(); ++i) config.cameras[i].enabled = false;
        if (!ip.empty()) { camera.device.ip = ip; camera.device.mac.clear(); }
        if (!mac.empty()) camera.device.mac = mac;
        if (exposure) camera.acquisition.exposure_ms = *exposure;
        if (gain) camera.acquisition.gain = *gain;
        if (frame_rate) camera.acquisition.frame_rate_hz = *frame_rate;
        // Same rule as the recording config: the exposure uses at most 90 % of the period.
        if (camera.acquisition.exposure_ms && camera.acquisition.frame_rate_hz &&
            !gox::ExposureFitsPeriod(*camera.acquisition.exposure_ms, *camera.acquisition.frame_rate_hz))
            return SnapshotFail(2, "exposure exceeds 90% of the frame period (900/frame_rate_hz ms)");
        camera.acquisition.trigger.mode = gox::TriggerMode::kFreerun;
        camera.acquisition.trigger.selector_entry = "FrameStart";
        camera.acquisition.trigger.exposure_active_output = false;
        config.sensor_trigger.enabled = false;
        config.ptp.enabled = false;
        // Apply after the YAML escape hatch. Bool/enum writes are required and
        // equality-checked by CameraController, and retained in device.json.
        // Disable the raw override's current trigger selector before FrameStart.
        camera.features.raw.push_back({"TriggerMode", "Off", true});
        camera.features.raw.push_back({"TriggerSelector", "FrameStart", true});
        camera.features.raw.push_back({"TriggerMode", "Off", true});
        camera.features.raw.push_back({"ExposureMode", "Timed", true});
        camera.features.raw.push_back({"AcquisitionMode", "Continuous", true});
        camera.features.raw.push_back({"GevIEEE1588", "false", false});
        if (stream_s) {
            // Live stream (Camera Tools focus assist): no frame limit. The GUI ends it
            // with SIGTERM; the duration bounds a stream whose GUI went away. 8-bit Bayer
            // for the higher frame rate; the snapshot YAML stays BayerRG12Packed.
            config.output.max_frames = 0;
            config.output.max_duration_s = *stream_s;
            camera.acquisition.pixel_format = gox::kFocusPixelFormat;
            camera.acquisition.focus_preview = true;
        } else {
            config.output.max_frames = 1;
            if (config.output.max_duration_s <= 0.0 || config.output.max_duration_s > 15.0)
                config.output.max_duration_s = 15.0;
        }
        auto cameras = ebus_driver::MakeGoxCameras(config, config_path, std::filesystem::absolute(output));
        if (!stream_s) return RunSnapshot(std::move(cameras.at(0)), 1);
        auto &options = cameras.at(0).options;
        // One record per segment: the GUI reads the newest frame and deletes the closed
        // segments behind it, so a stream keeps only a few frames on disk.
        options.recording.segment_records = 1;
        // Every frame closes and fsyncs a segment: give the writer about 2 s of slack at the
        // stream rate instead of the single-frame snapshot queue.
        options.queue_frames = std::max(options.queue_frames, gox::AutoQueueFrames(options.expected_rate_hz));
        return RunStream(std::move(cameras.at(0)));
    } catch (const std::exception &error) { return SnapshotFail(2, error.what()); }
}
