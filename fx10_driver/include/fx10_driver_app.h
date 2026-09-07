#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "data_type.h"
#include "driver_app.h"


class Fx10DriverApp final : public common::IDriverApp {
public:
    explicit Fx10DriverApp(const common::Config &config);

    ~Fx10DriverApp() override;

    bool Init(const std::function<bool()> &external_stop = {}) override;

    void Run() override;

    void Shutdown() override;

    // Silence since the last frame the transport delivered, for Main's no-data
    // watchdog. nullopt while no session is Streaming (bring-up, teardown) and
    // for the whole session under an external trigger not controlled by this
    // process. SensorSync-commanded periodic pulses are covered by the watchdog
    // as well.
    std::optional<std::uint64_t> MicrosSinceLastData() const override;

private:
    struct Impl; // fx10 types live in the .cpp only (keeps eBUS out of main)

    enum class BringUp { kOk, kStopped, kFailed };

    BringUp BringUpSession() const; // connect .. configure .. recorder params
    bool StartStreaming(); // EnviRecorder::Start + StreamReceiver::Start
    void MonitorLoop(); // 200 ms stop-condition poll + periodic stats
    void TeardownSession(); // stop -> classify -> finalize -> disconnect
    bool StopRequested() const;

    std::filesystem::path config_path_;
    std::filesystem::path data_folder_path_;
    std::function<bool()> external_stop_;
    std::unique_ptr<Impl> impl_;
    bool init_ok_{false};
    std::atomic<bool> shutdown_called_{false};
    // Steady-clock micros of the last delivered frame (or of AcquisitionStart
    // before the first one); 0 = the watchdog does not apply right now.
    // Published by the run thread's monitor loop, read by Main's poll loop —
    // impl_->session itself is not safe to touch from another thread.
    std::atomic<std::uint64_t> data_reference_us_{0};
};

