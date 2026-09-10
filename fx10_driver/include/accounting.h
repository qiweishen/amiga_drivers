#pragma once

#include <cstdint>

// Zero-loss accounting: BlockID continuity tracking and the run counter ledger.
// Each ledger field has one writer while streaming. Whole-ledger reads happen
// only after receiver.Stop() has joined both acquisition and recording workers.

namespace fx10 {
    // GVSP BlockID continuity. Wire IDs are 16-bit (GVSP 1.x: valid IDs 1..65535,
    // 0 is skipped on wrap: ... 65534, 65535, 1, 2 ...) unless the camera runs
    // GevGVSPExtendedIDMode (64-bit). kAuto starts 16-bit-compatible and switches to
    // 64-bit permanently once an ID above 65535 is observed.
    class BlockIdTracker {
    public:
        enum class Mode { kAuto, k16Bit, k64Bit };

        struct Observation {
            std::uint64_t gap_before = 0; // frames missing between previous and this one
            std::uint64_t first_missing = 0; // first absent BlockID when gap_before > 0
            bool anomaly = false; // duplicate / backwards / invalid ID (not a gap)
        };

        explicit BlockIdTracker(Mode mode = Mode::kAuto) : mode_(mode) {
        }

        Observation Observe(std::uint64_t block_id);

        [[nodiscard]] std::uint64_t Observed() const { return observed_; }
        [[nodiscard]] std::uint64_t TotalMissed() const { return total_missed_; }
        [[nodiscard]] std::uint64_t Anomalies() const { return anomalies_; }

    private:
        Mode mode_;
        bool first_ = true;
        bool saw_wide_ = false; // an ID > 65535 has been seen (kAuto -> 64-bit)
        std::uint64_t prev_ = 0;
        std::uint64_t observed_ = 0;
        std::uint64_t total_missed_ = 0;
        std::uint64_t anomalies_ = 0;
    };

    // Session counter ledger, logged at recorder stop. SINGLE-WRITER
    // rule per field (violating it double-counts):
    //   acquisition loop: retrieve_ok, retrieve_timeouts, op_errors, blockid_anomalies, recording_queue_drops
    //   recorder (via onFrame/onGap): blockid_gap_events, frames_missed_rx,
    //     size_mismatch_drops, frames_written, gap_lines_padded, bytes_written,
    //     write_errors, segments_finalized
    //   after worker join: recording_worker_unconfirmed (not additive with frames_written)
    //   control channel: missed_trigger_delta
    // The transport queues gap observations; only the recording worker calls
    // IFrameSink::OnGap and updates the recorder's gap counters.
    struct Counters {
        std::uint64_t retrieve_ok = 0;
        std::uint64_t retrieve_timeouts = 0; // normal idle in triggered mode
        std::uint64_t op_errors = 0; // buffer retrieved but invalid
        std::uint64_t blockid_gap_events = 0;
        std::uint64_t frames_missed_rx = 0; // sum of gap sizes
        std::uint64_t blockid_anomalies = 0;
        std::uint64_t recording_queue_drops = 0; // acquisition: usable frame could not enter bounded queue
        std::uint64_t recording_worker_unconfirmed = 0; // failed/unattempted frames; may include partly written data
        std::uint64_t size_mismatch_drops = 0;
        std::uint64_t frames_written = 0; // real frames on disk (excludes padding)
        std::uint64_t gap_lines_padded = 0; // synthetic zero lines (pad_zero policy)
        std::uint64_t bytes_written = 0; // .bil bytes on disk (incl. padded lines)
        std::uint64_t write_errors = 0;
        std::uint64_t segments_finalized = 0;
        // Camera-side missed-trigger counter delta over the run; -1 = node not mapped.
        std::int64_t missed_trigger_delta = -1;
    };

    enum class RunStatus { kClean, kDegraded };

    // CLEAN iff nothing was lost or irregular: no RX gaps, no op errors, no size
    // mismatches, no write errors, no BlockID anomalies, and no missed triggers
    // (unmapped counter = -1 does not count against CLEAN).
    RunStatus Classify(const Counters &counters);

    const char *ToString(RunStatus status);
} // namespace fx10
