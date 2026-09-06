#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "data_type.h"
#include "driver_app.h"


// Forward declarations to avoid pulling jai headers into the unified main.
namespace gox {
    class CaptureRunner;
    class StopController;
} // namespace gox


class GoxDriverApp final : public common::IDriverApp {
public:
    explicit GoxDriverApp(const common::Config &config);

    ~GoxDriverApp() override;

    // Load the YAML config and bring every enabled camera up
    [[nodiscard]] bool Init(const std::function<bool()> &external_stop = {}) override;

    void Run() override;

    // Graceful shutdown: stop all camera sessions
    void Shutdown() override;

    // Silence of the WORST camera, for Main's no-data watchdog; nullopt while
    // no camera qualifies (bring-up, teardown, or every camera on an external
    // trigger, where silence only means the pulses stopped).
    std::optional<std::uint64_t> MicrosSinceLastData() const override;

private:
    std::string config_path_; // resolved: exe_dir/../../ + gox_config_path
    std::string data_folder_path_; // <output>/<timestamp>/raw; the driver writes <that>/gox/<camera_id>/

    // Declaration order matters: stop_ must outlive runner_ (the runner's CameraSessions hold a StopController*)
    std::unique_ptr<gox::StopController> stop_;
    std::unique_ptr<gox::CaptureRunner> runner_;
    std::vector<std::string> camera_ids_; // enabled cameras, for the per-instance markers

    std::atomic<bool> shutdown_called_{false};
};


