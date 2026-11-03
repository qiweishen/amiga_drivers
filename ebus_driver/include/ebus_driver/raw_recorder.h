#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>


namespace ebus_driver {
    // An owning snapshot. metadata describes the original SDK buffer, not a decoded image.
    struct RawBuffer {
        nlohmann::json metadata;
        std::vector<std::uint8_t> payload;
    };

    struct RecorderOptions {
        std::filesystem::path directory;
        std::uint64_t segment_bytes = 2048ull * 1024 * 1024;
        std::uint64_t segment_records = 0; // zero disables the count limit
        std::uint64_t flush_bytes = 64ull * 1024 * 1024;
    };

    class RawRecorder {
    public:
        explicit RawRecorder(RecorderOptions options);

        ~RawRecorder();

        RawRecorder(const RawRecorder &) = delete;

        RawRecorder &operator=(const RawRecorder &) = delete;

        void Open(const nlohmann::json &device);

        // Single writer. A failed append latches failure and must not be retried.
        void Append(const RawBuffer &buffer);

        // May be called by the monitor concurrently with Append; separate event stream.
        void Event(const nlohmann::json &event);

        // Called after the writing worker joined; idempotent, reports close errors.
        void Close(bool complete, const std::string &reason);

        bool Failed() const { return failed_.load(); }
        std::uint64_t RecordsWritten() const { return records_.load(); }
        std::uint64_t FramesWritten() const { return frames_.load(); }

        // Call after the writing worker joins (segment bookkeeping is single-writer).
        nlohmann::json Summary() const;

        const std::filesystem::path &Directory() const { return options_.directory; }

    private:
        void OpenSegment();

        void CloseSegment();

        void Fail();

        RecorderOptions options_;
        int directory_fd_ = -1, data_fd_ = -1, events_fd_ = -1;
        std::uint64_t segment_ = 0, offset_ = 0, synced_ = 0, segment_records_ = 0;
        std::atomic<std::uint64_t> records_{0}, frames_{0}, bytes_{0};
        std::atomic<bool> failed_{false};
        mutable std::mutex events_mutex_;
        bool opened_ = false, closed_ = false;
    };
} // namespace ebus_driver
