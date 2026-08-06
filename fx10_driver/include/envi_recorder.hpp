#pragma once

#include <sys/types.h>  // ssize_t (used in WriteHook)

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "accounting.hpp"
#include "app_config.hpp"
#include "envi_header.hpp"
#include "frame.hpp"
#include "wavelengths.hpp"


// ENVI BIL recorder. One GVSP frame (bands x samples, row-major) is exactly one
// BIL line record, so the writer appends payloads verbatim. Per segment:
//   segment_NNNN.bil.part                             (streaming)
//   -> fdatasync -> rename -> segment_NNNN.hdr        (finalize; .hdr <=> valid)
// (run status and the counter ledger go to the log at stop).
// Line timing is NOT stored here: it lives in the SensorSync trigger log
// (sensor_trigger.log in the same session directory) and is matched to BIL
// line indices offline — which is why the pad_zero gap policy matters (it
// keeps line index <-> trigger sequence alignment across RX losses).
//
// Threading: all methods are called from the single acquisition thread
// (write-in-retrieve-loop topology); nothing here is thread-safe by design.

namespace fx10 {
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
        void start(const RecorderInit &init);

        // IFrameSink. Never throws: on I/O failure the recorder truncates to the last
        // complete line, finalizes what exists, and latches failed(); the acquisition
        // loop must check failed() and stop.
        void onFrame(const FrameView &frame) override;

        void onGap(std::uint64_t first_missing_block_id, std::uint64_t missing_count) override;

        // Finalize the open segment and log the final status line. Idempotent.
        // `reason` (e.g. "duration-reached", "sigint", "link-loss") is logged;
        // only the first call takes effect.
        void stop(const std::string &reason = std::string());

        bool failed() const override { return failed_; } // also polled by the transport
        const std::string &errorMessage() const { return error_message_; }
        const std::filesystem::path &sessionDir() const { return session_dir_; }
        std::uint64_t linesWrittenTotal() const { return global_line_index_; }
        std::uint32_t segmentIndex() const { return segment_index_; }

        // Test seam for injecting write failures (ENOSPC etc.). Signature of ::write.
        using WriteHook = std::function<ssize_t(int fd, const void *buf, std::size_t count)>;
        void setWriteHookForTest(WriteHook hook) { write_hook_ = std::move(hook); }

    private:
        void openSegment_(); // throws RecorderError
        void finalizeSegment_(); // never throws; truncates when failed_
        void removeEmptySegment_(); // drop a 0-line .part file silently
        void rotateIfNeeded_();

        // True when the line reached the file; rotation failures after a
        // successful write do not affect the return value.
        bool writeLine_(const std::uint8_t *data);

        bool writeAll_(int fd, const void *data, std::size_t size);

        // write -> fdatasync -> close -> rename; false (with .part cleaned up) on failure.
        bool writeFileAtomic_(const std::filesystem::path &part_path,
                              const std::filesystem::path &final_path, const std::string &content);

        void latchError_(const std::string &message);

        RecordingConfig config_;
        Counters &counters_;

        bool started_ = false;
        bool stopped_ = false;
        bool failed_ = false;
        std::string error_message_;

        RecorderInit init_;
        std::filesystem::path session_dir_;
        std::size_t line_bytes_ = 0;
        std::vector<std::uint8_t> zero_line_; // pad_zero policy only

        std::uint32_t segment_index_ = 0; // 1-based once a segment is open
        int data_fd_ = -1;
        std::filesystem::path data_part_path_;
        std::uint64_t lines_this_segment_ = 0;
        std::uint64_t data_bytes_this_segment_ = 0;
        std::string segment_start_iso_;

        std::uint64_t global_line_index_ = 0;
        std::uint32_t consecutive_size_mismatch_ = 0;

        WriteHook write_hook_;
    };
} // namespace fx10
