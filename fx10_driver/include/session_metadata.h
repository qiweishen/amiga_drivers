#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/types.h>
#include <nlohmann/json.hpp>


namespace fx10 {
    class MetadataError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // SDK-free serialization and sidecar I/O. No device reads or background threads.
    nlohmann::json RecordingContract(const std::string &pixel_format);
    void PublishMetadata(const std::filesystem::path &path, const nlohmann::json &document);

    // Single owner/thread. A failed append latches failure: never retry a partial row.
    // Each low-frequency row is synced; this is NOT used for the per-line index.
    class JsonlFile {
    public:
        JsonlFile() = default;
        ~JsonlFile();
        JsonlFile(const JsonlFile &) = delete;
        JsonlFile &operator=(const JsonlFile &) = delete;
        void Open(const std::filesystem::path &path);
        void Append(const nlohmann::json &row);
        void Close();
        bool IsOpen() const { return fd_ >= 0; }
        bool Failed() const { return failed_; }
        using WriteHook = std::function<ssize_t(int, const void *, std::size_t)>;
        void SetWriteHookForTest(WriteHook hook) { write_hook_ = std::move(hook); }
    private:
        int fd_ = -1;
        bool failed_ = false;
        std::uint64_t committed_bytes_ = 0;
        WriteHook write_hook_;
    };

    struct DeviceTelemetry {
        std::uint64_t host_realtime_ns = 0;
        std::uint64_t host_monotonic_ns = 0;
        std::uint64_t read_finished_monotonic_ns = 0;
        std::optional<double> temp_pcb_c, temp_fpga_c;
        std::optional<std::int64_t> missed_raw, missed_baseline;
        bool counter_regressed = false; // latched for the connection epoch
        bool counter_binding_verified = false;
    };

    // Unknown/reset/wrapped counters never become zero or a negative delta.
    std::optional<std::int64_t> MissedTriggerDelta(const DeviceTelemetry &sample);
    nlohmann::json BuildTelemetryJson(const DeviceTelemetry &sample, const std::string &phase);
}
