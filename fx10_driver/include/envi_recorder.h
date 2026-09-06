#pragma once

#include <sys/types.h>  // ssize_t (used in WriteHook)

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "accounting.h"
#include "app_config.h"
#include "envi_header.h"
#include "frame.h"
#include "wavelengths.h"


// ENVI BIL recorder. One GVSP frame (bands x samples, row-major) is exactly one
// BIL line record, so the writer appends payloads verbatim. Per segment:
//   segment_NNNN.bil.part                             (streaming)
//   -> fdatasync -> rename -> segment_NNNN.hdr        (finalize; .hdr <=> valid)
// (run status and the counter ledger go to the log at stop).
// Each segment also has a versioned .lines.csv: original BlockIDs, raw SDK
// timestamps, host receive clocks, rejected buffers, gaps and synthetic lines.
// BIL and index are finalized before the .hdr validity marker. A directory is
// one connection epoch. Leading losses / trigger association remain unknown
// until an independent anchor is established; line index is not a trigger ID.
//
// Threading: all methods are called from the single acquisition thread
// (write-in-retrieve-loop topology); nothing here is thread-safe by design.

namespace fx10 {
    // Why a recording stopped (mapped to an exit code by the driver)
    enum class ErrorKind { kNone, kIo, kGeometry, kOther };

    class RecorderError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct RecorderInit {
        std::uint32_t samples = 0; // spatial pixels per line
        std::uint32_t bands = 0; // spectral bands (frame height)
        std::uint32_t bytes_per_pixel = 2; // 2 = Mono12/Mono10 in uint16, 1 = Mono8
        EnviDataType data_type = EnviDataType::kUint16;
        Wavelengths wavelengths; // resolved axis; nm may be empty (omit from .hdr)
        std::string description; // camera/app metadata for the .hdr description
        std::string pixel_format; // reported format before lossless unpacking
        double expected_frame_rate_hz = 0.0; // preview window only, never a timestamp
    };

    // Create <output_dir>/<base_name>_<utc_stamp>/ ; on collision appends _1.._999.
    // Throws RecorderError when the directory cannot be created.
    std::filesystem::path createSessionDir(const std::filesystem::path &output_dir,
                                           const std::string &base_name,
                                           const std::string &utc_stamp);

    class EnviRecorder final : public IFrameSink {
    public:
        // `counters` is the shared session ledger; the recorder is the SOLE writer of
        // the gap/write/size-mismatch fields (the transport reports gaps via onGap and
        // must not increment those counters itself — single-writer rule).
        EnviRecorder(const RecordingConfig &config, Counters &counters);

        ~EnviRecorder() override;

        EnviRecorder(const EnviRecorder &) = delete;

        EnviRecorder &operator=(const EnviRecorder &) = delete;

        // Creates the session directory and opens segment 1. Throws RecorderError.
        void Start(const RecorderInit &init);

        // IFrameSink. Never throws: on I/O failure the recorder truncates to the last
        // complete line, finalizes what exists, and latches Failed(); the acquisition
        // loop must check Failed() and stop.
        void OnFrame(const FrameView &frame) override;

        void OnGap(std::uint64_t first_missing_block_id, std::uint64_t missing_count) override;
        void OnRejected(std::uint64_t block_id, const char *reason) override;

        // Finalize the open segment and log the final status line. Idempotent.
        // `reason` (e.g. "duration-reached", "sigint", "link-loss") is logged;
        // only the first call takes effect.
        void Stop(const std::string &reason = std::string());

        // Atomic: written by the acquisition thread (which owns onFrame/onGap),
        // polled by the monitor thread every 200 ms and by the transport.
        [[nodiscard]] bool Failed() const override { return failed_.load(std::memory_order_acquire); }
        [[nodiscard]] const std::string &ErrorMessage() const { return error_message_; }

        // Why the recorder failed, so the caller does not have to classify by
        // matching substrings of a log message.
        [[nodiscard]] ErrorKind GetErrorKind() const { return error_kind_; }
        [[nodiscard]] const std::filesystem::path &SessionDir() const { return session_dir_; }
        [[nodiscard]] std::uint64_t LinesWrittenTotal() const { return global_line_index_; }

        // Thread-safe mirror of Counters::frames_written (the ledger itself is
        // plain: single-writer, acquisition thread). The main thread polls this
        // for the periodic [Statistics] line's write rate.
        [[nodiscard]] std::uint64_t FramesWrittenTotal() const { return frames_written_.load(std::memory_order_relaxed); }

        // Test seam for injecting write failures (ENOSPC etc.). Signature of ::write.
        using WriteHook = std::function<ssize_t(int fd, const void *buf, std::size_t count)>;
        void SetWriteHookForTest(WriteHook hook) { write_hook_ = std::move(hook); }

        // Test seam for the durability calls. Signature of ::fdatasync; the
        // periodic flush cadence is otherwise unobservable from outside.
        using SyncHook = std::function<int(int fd)>;
        void SetSyncHookForTest(SyncHook hook) { sync_hook_ = std::move(hook); }

    private:
        void OpenSegment(); // throws RecorderError
        void FinalizeSegment(); // never throws; truncates when failed_
        void RemoveEmptySegment(); // drop a 0-line .part file silently
        void RotateIfNeeded();

        // True when the line reached the file; rotation failures after a
        // successful write do not affect the return value.
        bool WriteLine(const std::uint8_t *data, const FrameView *frame = nullptr);
        bool WriteIndex(const char *event, std::uint64_t block_id, std::uint64_t count,
                        const FrameView *frame = nullptr);

        bool WriteAll(int fd, const void *data, std::size_t size);

        int Sync(int fd); // ::fdatasync, or the test hook

        // Latch a failure raised INSIDE finalizeSegment_. latchError_ finalizes
        // the segment, so it would recurse; this sets the fields directly.
        void LatchFinalizeFailure(const std::string &message);

        // write -> fdatasync -> close -> rename; false (with .part cleaned up) on failure.
        bool WriteFileAtomic(const std::filesystem::path &part_path,
                              const std::filesystem::path &final_path, const std::string &content);

        void LatchError(const std::string &message, ErrorKind kind);

        RecordingConfig config_;
        Counters &counters_;

        bool started_ = false;
        bool stopped_ = false;
        std::atomic<bool> failed_{false};
        std::string error_message_;
        ErrorKind error_kind_ = ErrorKind::kNone;

        RecorderInit init_;
        std::filesystem::path session_dir_;
        std::size_t line_bytes_ = 0;
        std::vector<std::uint8_t> zero_line_; // pad_zero policy only

        std::uint32_t segment_index_ = 0; // 1-based once a segment is open
        int data_fd_ = -1;
        int index_fd_ = -1;
        std::filesystem::path data_part_path_;
        std::filesystem::path index_part_path_;
        std::uint64_t index_bytes_ = 0; // complete CSV records; rollback point on short writes
        std::uint64_t index_records_ = 0;
        std::uint64_t lines_this_segment_ = 0;
        std::uint64_t data_bytes_this_segment_ = 0;
        std::uint64_t last_flush_bytes_ = 0; // periodic fdatasync cadence
        std::string segment_start_iso_;

        std::uint64_t global_line_index_ = 0;
        std::uint32_t consecutive_size_mismatch_ = 0;
        std::atomic<std::uint64_t> frames_written_{0}; // main-thread-readable copy of counters_.frames_written

        WriteHook write_hook_;
        SyncHook sync_hook_;
    };
} // namespace fx10
