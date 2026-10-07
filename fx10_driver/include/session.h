#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>

namespace common { class SensorSyncHub; }

namespace fx10 {
    // One acquisition session; owns camera, recording, telemetry and teardown.
    // One-shot: construct a new Session for a subsequent acquisition epoch.
    // Prepare/Run/Shutdown belong to one owner at a time. Liveness is the only
    // cross-thread query; final statistics and Failed are read after Run joins.
    // SDK types stay inside Impl, so callers need no eBUS headers.
    class Session {
    public:
        Session(std::filesystem::path config_path, std::filesystem::path data_folder_path,
                std::shared_ptr<common::SensorSyncHub> sync, std::function<bool()> stop_requested);
        ~Session();

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        // Connect/configure and prepare recording metadata; does not start acquisition.
        bool Prepare();
        // Start acquisition, monitor stop/failure conditions, then drain the session.
        void Run();
        // Also safe after partial Prepare or when Run never started; idempotent.
        void Shutdown();

        [[nodiscard]] bool Failed() const;
        [[nodiscard]] std::optional<std::uint64_t> MicrosSinceLastData() const;
        [[nodiscard]] nlohmann::ordered_json FinalStatistics() const;

    private:
        struct Impl;
        enum class BringUp { kOk, kStopped, kFailed };
        BringUp BringUpSession() const;
        bool StartStreaming();
        void MonitorLoop();
        void TeardownSession();
        bool StopRequested() const;

        std::filesystem::path config_path_;
        std::filesystem::path data_folder_path_;
        std::function<bool()> external_stop_;
        std::unique_ptr<Impl> impl_;
        std::atomic<bool> shutdown_requested_{false};
        std::atomic<bool> shutdown_called_{false};
        // Published by the session owner; never expose its Connection cross-thread.
        std::atomic<std::uint64_t> data_reference_us_{0};
    };
} // namespace fx10
