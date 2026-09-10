#pragma once

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>


namespace common {
    class DriversJson {
    public:
        // path: usually "<data_folder>/drivers.json"
        explicit DriversJson(std::string path);

        void SetMeta(const std::string &operator_name, const std::string &field_name);

        // Run header; call before WriteRunning()
        void SetRun(const std::string &timestamp, const std::string &output_directory);

        // Build identity of the whole AmigaDrivers process (project version +
        // git SHA baked at configure time); call before WriteRunning()
        void SetVersion(const std::string &version, const std::string &git_sha);

        // One entry per driver: {"enabled": ..., "config": <path as configured>}
        void AddDriver(const std::string &name, bool enabled, const std::string &config_path);
        void AddDriverResult(const std::string &name, bool failed, const nlohmann::ordered_json &statistics);

        // Writes the document with run.status = "running"
        bool WriteRunning();

        // Versioned operational state, written only on lifecycle transitions.
        // Main owns this object; never call from driver receive/write threads.
        bool UpdateLifecycle(const std::string &phase, bool configuration_read,
                             const nlohmann::ordered_json &sensors);

        // Sets run.status / run.ended / run.duration_s and writes the final
        // document. status: "completed" | "interrupted (signal N)" | ...
        bool Finalize(const std::string &status);

    private:
        bool Write_();

        std::string path_;
        std::string control_path_;
        nlohmann::ordered_json doc_;
        std::chrono::steady_clock::time_point start_;
    };
} // namespace common
