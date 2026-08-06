/// @file drivers_json.h
/// @brief Run-level summary for the whole AmigaDrivers process, written to
/// <data_folder>/drivers.json. One small file per run instead of per-driver
/// session metadata: when the run started/ended, how it ended, and which
/// drivers ran with which config.

#ifndef COMMON_DRIVERS_JSON_H
#define COMMON_DRIVERS_JSON_H

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>


namespace Common {
    // Written once at startup (status "running") and atomically replaced at
    // shutdown (temp file + fsync + rename), so the file is complete and
    // parseable at any moment — even after kill -9 it still documents the run.
    // All writes are best-effort: failures log a warning and never throw (a
    // metadata file must not take down data capture). Not thread-safe: call
    // from the main thread only.
    class DriversJson {
    public:
        // path: usually "<data_folder>/drivers.json"
        explicit DriversJson(std::string path);

        // Run header; call before WriteRunning()
        void SetRun(const std::string &timestamp, const std::string &output_directory, bool logging_enabled);

        // Build identity of the whole AmigaDrivers process (project version +
        // git SHA baked at configure time); call before WriteRunning()
        void SetVersion(const std::string &version, const std::string &git_sha);

        // One entry per driver: {"enabled": ..., "config": <path as configured>}
        void AddDriver(const std::string &name, bool enabled, const std::string &config_path);

        // Writes the document with run.status = "running"
        void WriteRunning();

        // Sets run.status / run.ended / run.duration_s and writes the final
        // document. status: "completed" | "interrupted (signal N)" | ...
        void Finalize(const std::string &status);

    private:
        void Write_();

        std::string path_;
        nlohmann::ordered_json doc_;
        std::chrono::steady_clock::time_point start_;
    };
} // namespace Common

#endif	// COMMON_DRIVERS_JSON_H
