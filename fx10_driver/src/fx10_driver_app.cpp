#include "fx10_driver_app.h"

#include "driver_markers.h"
#include "logger.h"
#include "session.h"

namespace {
    common::DriverLog g_log{std::string(common::Markers::kModuleFx10)};
}

Fx10DriverApp::Fx10DriverApp(const common::Config &config)
    : config_path_(config.fx10_config_path), data_folder_path_(config.data_folder_path),
      sync_(config.sensor_sync) {}

Fx10DriverApp::~Fx10DriverApp() {
    Shutdown();
}

bool Fx10DriverApp::StopRequested() const {
    return terminate_.load(std::memory_order_acquire) || (external_stop_ && external_stop_());
}

bool Fx10DriverApp::Init(const std::function<bool()> &external_stop) {
    external_stop_ = external_stop;
    session_ = std::make_unique<fx10::Session>(config_path_, data_folder_path_, sync_,
                                             [this] { return StopRequested(); });
    init_ok_ = session_->Prepare();
    if (!init_ok_) {
        if (session_->Failed()) MarkFailed();
        return false;
    }
    g_log.Info("{}", common::Markers::kFx10Initialized);
    return true;
}

void Fx10DriverApp::Run() {
    if (!session_ || !init_ok_) {
        RequestFailure();
        g_log.Error("FX10 Run called without a prepared session");
        return;
    }
    session_->Run();
    if (session_->Failed()) MarkFailed();
    // Completion, configured limits and internal failures all end this rig run.
    terminate_.store(true, std::memory_order_release);
}

void Fx10DriverApp::Shutdown() {
    if (shutdown_called_.exchange(true)) return;
    terminate_.store(true, std::memory_order_release);
    if (!session_) return;
    session_->Shutdown();
    if (session_->Failed()) MarkFailed();
    if (!init_ok_) return; // initialization diagnostics were already emitted
    if (HasFailed()) {
        g_log.Error("{}", common::Markers::kFx10SessionIssues);
    } else {
        g_log.Info("{}", common::Markers::kFx10Shutdown);
    }
}

std::optional<std::uint64_t> Fx10DriverApp::MicrosSinceLastData() const {
    return session_ ? session_->MicrosSinceLastData() : std::nullopt;
}

nlohmann::ordered_json Fx10DriverApp::FinalStatistics() const {
    return session_ ? session_->FinalStatistics() : nlohmann::ordered_json::object();
}
