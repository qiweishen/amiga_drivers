#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

#include "data_type.h"
#include "driver_app.h"


// Forward declaration to keep Qt/asterx headers out of the unified main.
namespace asterx {
    struct AppConfig;
}


class AsterxDriverApp final : public common::IDriverApp {
public:
    explicit AsterxDriverApp(const common::Config &config);

    ~AsterxDriverApp() override;

    bool Init(const std::function<bool()> &external_stop = {}) override;

    void Run() override;

    void Shutdown() override;
    nlohmann::ordered_json FinalStatistics() const override { return final_statistics_; }

    // Silence since the last recorded SBF block, for Main's no-data watchdog.
    // nullopt outside the Recording state: connecting, warming up and the
    // reconnect backoff are all silences this driver expects and handles
    // itself (its own 30 s QTimer triggers a RECONNECT, not a run abort).
    [[nodiscard]] std::optional<std::uint64_t> MicrosSinceLastData() const override;

private:
    nlohmann::ordered_json final_statistics_ = nlohmann::ordered_json::object();
    enum class BringUp { Pending, Recording, Failed };

    void QtThreadMain(asterx::AppConfig cfg); // Qt world lives entirely in here
    void NotifyBringUp(BringUp outcome); // Pending -> outcome (first wins), notify_all
    BringUp BringUpOutcome(); // thread-safe read of bring_up_

    std::filesystem::path config_path_;
    std::filesystem::path data_folder_path_;

    std::thread qt_thread_;
    std::mutex bring_up_mutex_;
    std::condition_variable bring_up_cv_;
    BringUp bring_up_{BringUp::Pending};

    std::atomic<bool> shutdown_called_{false};

    // Steady-clock micros of the last recorded SBF block, or of the moment
    // recording started; 0 = the watchdog does not apply. Written by the Qt
    // thread (Session::PublishLiveness), read by Main's poll loop. It lives
    // here, not on the Session, because the Session is a stack object on the Qt
    // thread and must not be reachable from another thread.
    std::atomic<std::uint64_t> data_reference_us_{0};
};
