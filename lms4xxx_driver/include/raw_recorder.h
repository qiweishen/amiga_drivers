#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "raw_record.h"

namespace lms4xxx {
    // Single-use asynchronous recorder. BIN contains unmodified telegrams;
    // the matching CSV and JSON describe offsets and acquisition provenance.
    class RawRecorder {
    public:
        struct Config {
            std::string output_path; // scan_<id>_<timestamp>.bin -> ..._000.bin
            std::string instance;
            std::string session_timestamp;
            nlohmann::ordered_json metadata = nlohmann::ordered_json::object();
            std::size_t queue_capacity = 512;
            std::uint64_t max_file_bytes = 1ULL * 1024 * 1024 * 1024; // 0 disables splitting
            std::uint32_t flush_interval_ms = 1000;
        };

        struct Statistics {
            std::uint64_t frames_queued = 0; // all complete telegrams, including bad checksums
            std::uint64_t frames_written = 0; // BIN and CSV append completed; not per-frame durability
            std::uint64_t frames_dropped = 0;
            std::uint64_t scans_queued = 0; // subset identified as LMDscandata
            std::uint64_t scans_written = 0;
            std::uint64_t bytes_written = 0; // complete BIN telegram bytes, excludes CSV/JSON
            std::uint64_t files_created = 0; // BIN splits
        };

        explicit RawRecorder(Config config);
        ~RawRecorder();
        RawRecorder(const RawRecorder &) = delete;
        RawRecorder &operator=(const RawRecorder &) = delete;

        bool Start();
        // Takes ownership without waiting for disk; a full queue fails the run.
        void OnFrame(RawRecord &&record);
        // Call after the producer stops. Drains accepted records, then finalizes.
        void Stop();
        [[nodiscard]] bool HasFailed() const;
        [[nodiscard]] Statistics GetStatistics() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lms4xxx
