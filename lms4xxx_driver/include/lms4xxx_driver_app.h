#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "app_config.h"
#include "data_type.h"
#include "driver_app.h"
#include "driver.h"
#include "scan_record_writer.h"


// One app per enabled lidar[] entry; main.cpp loads the yaml once and fans out.
class Lms4xxxDriverApp final : public common::IDriverApp {
public:
    Lms4xxxDriverApp(const common::Config &run, const lms4xxx::AppConfig &config, const lms4xxx::LidarConfig &lidar);

    ~Lms4xxxDriverApp() override;

    // external_stop unused: connect/configure are timeout-bounded
    bool Init(const std::function<bool()> &external_stop = {}) override;

    void Run() override;

    void Shutdown() override;
    nlohmann::ordered_json FinalStatistics() const override { return final_statistics_; }

    // Silence since the last scan telegram; nullopt until `sEN LMDscandata 1` is acknowledged.
    // Before the first telegram the reference is the moment scanning started.
    std::optional<std::uint64_t> MicrosSinceLastData() const override;

private:
    struct Impl {
        std::unique_ptr<lms4xxx::Lms4xxxDriver> driver;
        std::unique_ptr<lms4xxx::ScanRecordWriter> writer;
        std::chrono::steady_clock::time_point scan_start{};
        lms4xxx::TelemetrySample last_telemetry; // most recent reading for the status line
    };

    std::unique_ptr<Impl> impl_;
    std::string instance_name_;
    nlohmann::ordered_json final_statistics_ = nlohmann::ordered_json::object();
    lms4xxx::DriverConfig driver_config_;
    lms4xxx::OutputConfig output_;
    double telemetry_interval_s_;
    double stats_interval_s_;
    std::string data_folder_path_;
    std::string timestamp_;
    std::string config_path_;

    // Steady-clock micros at which streaming started; 0 = not streaming (MicrosSinceLastData -> nullopt).
    // Written by Run(), read from Main's poll thread.
    std::atomic<std::uint64_t> scan_start_us_{0};
};
