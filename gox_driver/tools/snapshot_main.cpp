// GUI-compatible one-camera preview command using the common eBUS RAW engine.
#include "../include/app_config.h"
#include "ebus_driver/tool_support.h"

#include <cstdio>
#include <optional>
#include <string>

namespace {
void Usage(const char *program) {
    std::printf("Usage: %s --config <snapshot.yaml> --out <dir> [overrides]\n"
        "Captures one GOX frame in freerun (PTP and SensorSync off) under <dir>/<camera_id>.\n"
        "Overrides: --ip <address> | --mac <address>, --exposure-ms <value>, --gain <value>\n"
        "Last stdout line: SNAPSHOT: OK <camera_dir> or SNAPSHOT: FAIL <code> <reason>\n", program);
}
}

int main(int argc, char **argv) {
    using namespace ebus_driver::tool;
    std::string config_path, output, ip, mac;
    std::optional<double> exposure, gain;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { Usage(argv[0]); return 0; }
        if (i + 1 >= argc) return SnapshotFail(2, "missing value for " + arg);
        const char *value = argv[++i];
        if (arg == "--config") config_path = value;
        else if (arg == "--out") output = value;
        else if (arg == "--ip") ip = value;
        else if (arg == "--mac") mac = value;
        else if (arg == "--exposure-ms" || arg == "--gain") {
            double number = 0;
            if (!ParseDouble(value, number) ||
                (arg == "--exposure-ms" ? number < 0.0 : number < 1.0 || number > 126.0))
                return SnapshotFail(2, "invalid value for " + arg);
            (arg == "--gain" ? gain : exposure) = number;
        } else return SnapshotFail(2, "unknown option " + arg);
    }
    if (config_path.empty() || output.empty()) return SnapshotFail(2, "--config and --out are required");
    if (!ip.empty() && !mac.empty()) return SnapshotFail(2, "--ip and --mac are mutually exclusive");
    try {
        auto config = gox::LoadAppConfig(config_path);
        auto &camera = config.cameras.at(0);
        camera.enabled = true;
        for (std::size_t i = 1; i < config.cameras.size(); ++i) config.cameras[i].enabled = false;
        if (!ip.empty()) { camera.device.ip = ip; camera.device.mac.clear(); }
        if (!mac.empty()) camera.device.mac = mac;
        if (exposure) camera.acquisition.exposure_ms = *exposure;
        if (gain) camera.acquisition.gain = *gain;
        if (camera.acquisition.exposure_ms && camera.acquisition.frame_rate_hz &&
            *camera.acquisition.exposure_ms >= 1000.0 / *camera.acquisition.frame_rate_hz)
            return SnapshotFail(2, "exposure does not fit the configured frame period");
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
        config.output.max_frames = 1;
        if (config.output.max_duration_s <= 0.0 || config.output.max_duration_s > 15.0)
            config.output.max_duration_s = 15.0;
        auto cameras = ebus_driver::MakeGoxCameras(config, config_path, std::filesystem::absolute(output));
        return RunSnapshot(std::move(cameras.at(0)), 1);
    } catch (const std::exception &error) { return SnapshotFail(2, error.what()); }
}
