// White/dark phases use the same unified engine as normal recording. Each phase
// owns a separate verified connection and preserves native SDK bytes in RAW.
#include "../include/app_config.h"
#include "../include/apply_plan.h"
#include "ebus_driver/tool_support.h"
#include "logger.h"
#include "sensor_sync_hub.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unistd.h>

namespace {
using Json = nlohmann::json;
namespace Tool = ebus_driver::tool;

void Marker(const std::string &message) {
    std::printf("REFERENCE: %s\n", common::StringUtil::OneLine(message).c_str());
    std::fflush(stdout);
}
void CheckCancelled() {
    if (Tool::signal != 0) throw std::runtime_error("reference collection interrupted");
}
// No-clobber publication of tool manifests/configuration. RAW data files remain
// entirely owned by EbusCameraDriver/RawRecorder.
void PublishText(const std::filesystem::path &path, const std::string &text) {
    auto part = path;
    part += ".part";
    int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) throw std::runtime_error("create " + part.string() + ": " + std::strerror(errno));
    try {
        std::size_t written = 0;
        while (written < text.size()) {
            const auto n = ::write(fd, text.data() + written, text.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("write " + part.string());
            written += static_cast<std::size_t>(n);
        }
        int synced;
        do { synced = ::fdatasync(fd); } while (synced != 0 && errno == EINTR);
        if (synced != 0) throw std::runtime_error("sync " + part.string());
        const int closed = ::close(fd);
        fd = -1;
        if (closed != 0) throw std::runtime_error("close " + part.string());
        if (::link(part.c_str(), path.c_str()) != 0) throw std::runtime_error("publish " + path.string());
        if (::unlink(part.c_str()) != 0) throw std::runtime_error("unlink " + part.string());
        const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) throw std::runtime_error("open manifest directory");
        int result;
        do { result = ::fsync(directory); } while (result != 0 && errno == EINTR);
        ::close(directory);
        if (result != 0) throw std::runtime_error("sync manifest directory");
    } catch (...) {
        if (fd >= 0) ::close(fd);
        ::unlink(part.c_str());
        throw;
    }
}
void PublishJson(const std::filesystem::path &path, const Json &document) {
    PublishText(path, document.dump(2) + "\n");
}

Json RequiredNode(const Json &device, const char *group, const char *name) {
    const auto &node = device.at(group).at(name);
    if (node.value("status", std::string()) != "ok" || !node.contains("value") || node.at("value").is_null())
        throw std::runtime_error(std::string("reference requires a verified ") + name + " readback");
    return node.at("value");
}

Json PhaseSignature(const Json &device, const fx10::AppConfig &config) {
    Json signature = Json::object();
    signature["serial"] = RequiredNode(device, "identity", "DeviceSerialNumber");
    if (!signature["serial"].is_string() || signature["serial"].get<std::string>().empty())
        throw std::runtime_error("reference camera serial is empty");
    signature["model"] = RequiredNode(device, "identity", "DeviceModelName");
    for (const char *name : {"Width", "Height", "OffsetX", "OffsetY", fx10::node::kPixelFormat,
            fx10::node::kSpatialBinning, fx10::node::kSpectralBinning, fx10::node::kStatusLine,
            fx10::node::kMroiEnable, fx10::node::kExposureMode, fx10::node::kFrameRateEnable,
            fx10::node::kTriggerSelector, fx10::node::kTriggerMode})
        signature["imaging"][name] = RequiredNode(device, "final_readback", name);
    const bool external = config.acquisition.trigger.mode == fx10::TriggerMode::kExternal;
    if (!external || config.acquisition.trigger.exposure_control == fx10::ExposureControl::kCamera)
        signature["imaging"][fx10::node::kExposureTime] = RequiredNode(device, "final_readback", fx10::node::kExposureTime);
    if (external) {
        for (const char *name : {fx10::node::kTriggerSource, fx10::node::kTriggerActivation, fx10::node::kTriggerDelay})
            signature["imaging"][name] = RequiredNode(device, "final_readback", name);
    } else signature["imaging"][fx10::node::kFrameRate] = RequiredNode(device, "final_readback", fx10::node::kFrameRate);
    // Ordered applied entries preserve selector-scoped MROI and operator writes
    // that cannot be recovered from a single final selector position.
    signature["applied"] = device.at("applied");
    signature["wavelengths_nm"] = device.at("profile").at("wavelengths_nm");
    signature["fwhm_nm"] = device.at("profile").at("fwhm_nm");
    signature["calibration"] = device.at("profile").at("calibration");
    signature["scope"] = "identity and available explicit imaging readbacks; AIE/undocumented factory state and shutter position remain unverified";
    return signature;
}

void CapturePhase(const char *phase, const fx10::AppConfig &saved,
                  const std::filesystem::path &config_snapshot, const std::string &sensor_port,
                  const std::filesystem::path &root, std::optional<Json> &white_signature, Json &result) {
    CheckCancelled();
    auto config = saved;
    config.recording.max_duration_s = saved.reference.duration_s;
    config.recording.max_frames = 0;
    const auto phase_parent = root / "raw" / "fx10" / phase;
    const auto directory = Tool::TimestampedPath(phase_parent, "fx10");
    // Hub registration may create the observation file's parent. Keep that
    // separate from the camera directory RawRecorder creates exclusively.
    std::shared_ptr<common::SensorSyncHub> sync;
    if (config.sensor_trigger.enabled) {
        std::filesystem::create_directories(phase_parent);
        sync = std::make_shared<common::SensorSyncHub>(phase_parent / "sensor_trigger.log", sensor_port);
    }
    auto definition = ebus_driver::MakeFx10Camera(config, config_snapshot, directory, std::string(phase) == "dark");
    ebus_driver::EbusCameraDriver driver(std::move(definition), sync);
    result = {{"phase", phase}, {"status", "incomplete"},
        {"session_dir", std::filesystem::relative(directory, root).generic_string()},
        {"recording_format", "ebus-raw-v1"}, {"requested_duration_s", config.reference.duration_s},
        {"connection_scope", "separate verified phases"}, {"shutter_state_verified", false}};
    try {
        if (!driver.Init([] { return Tool::signal != 0; })) {
            driver.Shutdown();
            throw std::runtime_error(driver.FinalStatistics().value("error", std::string("reference initialization failed")));
        }
        std::ifstream metadata(directory / "device.json");
        if (!metadata) throw std::runtime_error("reference device metadata is missing");
        Json device;
        metadata >> device;
        const auto signature = PhaseSignature(device, config);
        if (white_signature && *white_signature != signature)
            throw std::runtime_error("white/dark serial, geometry, PixelFormat or imaging readbacks differ; reference pair rejected");
        if (!white_signature) white_signature = signature;
        result["verified_configuration"] = signature;
        Marker(std::string("PHASE ") + phase);
        result["run_requested_monotonic_ns"] = common::TimeUtil::MonotonicNowNs();
        driver.Run();
        driver.Shutdown();
        CheckCancelled();
        const auto statistics = driver.FinalStatistics();
        result["statistics"] = statistics;
        result["frames_written"] = Tool::Count(statistics, "frames_written");
        result["records_written"] = Tool::Count(statistics, "records_written");
        result["bytes_written"] = Tool::Count(statistics, "payload_bytes_written");
        if (driver.HasFailed()) throw std::runtime_error(statistics.value("error", std::string("reference capture failed")));
        if (Tool::Count(statistics, "frames_written") == 0)
            throw std::runtime_error("reference contains no valid frames; check the configured trigger source");
        result["status"] = "completed";
        result["finalized_monotonic_ns"] = common::TimeUtil::MonotonicNowNs();
        result["duration_semantics"] = "host monotonic window from AcquisitionStart command; startup/drain buffers retain native timestamps";
        if (sync) result["sensor_sync"] = sync->Summary();
        PublishJson(directory / "reference_phase.json", result);
    } catch (const std::exception &error) {
        driver.Shutdown();
        const auto statistics = driver.FinalStatistics();
        result["status"] = Tool::signal != 0 ? "interrupted" : "failed";
        result["error"] = error.what();
        result["frames_written"] = Tool::Count(statistics, "frames_written");
        result["records_written"] = Tool::Count(statistics, "records_written");
        result["statistics"] = statistics;
        if (std::filesystem::is_directory(directory)) {
            try { PublishJson(directory / "reference_phase.json", result); } catch (...) {}
        }
        throw;
    }
}
}

int main(int argc, char **argv) {
    std::string config_path, output, sensor_port;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::puts("Usage: fx10_reference --config <saved FX10 YAML> --out <root> [--sensor-port <tty>]\n"
                "White/open then dark/closed, each reference.duration_s; native ebus-raw-v1 records.\n"
                "Each phase has a separate connection; verified device/imaging readbacks must match.");
            return 0;
        }
        if ((arg != "--config" && arg != "--out" && arg != "--sensor-port") || i + 1 >= argc) {
            Marker("FAIL invalid arguments"); return 2;
        }
        (arg == "--config" ? config_path : arg == "--out" ? output : sensor_port) = argv[++i];
    }
    if (config_path.empty() || output.empty()) { Marker("FAIL --config and --out are required"); return 2; }
    Tool::InstallSignals();
    std::filesystem::path root;
    Json summary = {{"format", "fx10-reference-v1"}, {"status", "incomplete"},
        {"phases", Json::array()}, {"config_source", config_path}, {"recording_format", "ebus-raw-v1"},
        {"connection_scope", "separate verified phases"},
        {"white_target", "operator positions illuminated white reference before starting"}};
    try {
        std::ifstream input(config_path);
        if (!input) throw std::runtime_error("cannot open FX10 config");
        const std::string yaml{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad()) throw std::runtime_error("cannot read FX10 config");
        const auto config = fx10::LoadAppConfigText(yaml);
        if (config.sensor_trigger.enabled && sensor_port.empty())
            throw std::runtime_error("SensorSync reference requires --sensor-port");
        summary["phase_duration_s"] = config.reference.duration_s;
        Marker("CONFIG " + Json{{"duration_s", config.reference.duration_s}}.dump());
        root = Tool::TimestampedPath(output, "reference");
        std::filesystem::create_directories(root.parent_path());
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("reference directory already exists");
        Marker("SESSION " + root.string());
        const auto config_directory = root / "raw" / "config";
        std::filesystem::create_directories(config_directory);
        const auto config_snapshot = config_directory / "config-fx10.yaml";
        PublishText(config_snapshot, yaml);
        common::Logger::Config logging;
        logging.log_file = (root / "reference.log").string();
        common::Logger::Init(logging, "FX10Reference");
        summary["config_snapshot"] = "raw/config/config-fx10.yaml";
        summary["overrides"] = {{"output.max_duration_s", config.reference.duration_s}, {"output.max_frames", 0}};
        PublishJson(root / "reference.request.json", summary);
        std::optional<Json> white_signature;
        for (const char *phase : {"white", "dark"}) {
            summary["phases"].push_back(Json::object());
            CapturePhase(phase, config, config_snapshot, sensor_port, root, white_signature, summary["phases"].back());
        }
        CheckCancelled();
        summary["status"] = "completed";
        summary["configuration_match_verified"] = true;
        PublishJson(root / "reference.json", summary);
        Marker("OK " + root.string());
        return 0;
    } catch (const std::exception &error) {
        summary["status"] = Tool::signal != 0 ? "interrupted" : "failed";
        summary["error"] = error.what();
        if (!root.empty()) {
            try { PublishJson(root / "reference.json", summary); } catch (...) {}
        }
        Marker("FAIL " + std::string(error.what()));
        return Tool::signal != 0 ? 130 : 3;
    }
}
