#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "data_type.h"
#include "driver_app.h"

namespace fx10 { class Session; }
namespace common { class SensorSyncHub; }

// IDriverApp adapter only. The FX10 Session owns device and recording lifetimes.
class Fx10DriverApp final : public common::IDriverApp {
public:
    explicit Fx10DriverApp(const common::Config &config);
    ~Fx10DriverApp() override;

    bool Init(const std::function<bool()> &external_stop = {}) override;
    void Run() override;
    void Shutdown() override;
    std::optional<std::uint64_t> MicrosSinceLastData() const override;
    nlohmann::ordered_json FinalStatistics() const override;

private:
    bool StopRequested() const;

    std::filesystem::path config_path_;
    std::filesystem::path data_folder_path_;
    std::shared_ptr<common::SensorSyncHub> sync_;
    std::function<bool()> external_stop_;
    // Created during Init, then retained through Shutdown; only its atomic
    // liveness snapshot is queried concurrently by the rig monitor.
    std::unique_ptr<fx10::Session> session_;
    bool init_ok_{false};
    std::atomic<bool> shutdown_called_{false};
};
