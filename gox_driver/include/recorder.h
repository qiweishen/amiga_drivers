#pragma once

// Segment-file recorder: one instance per camera, driven from that camera's
// writer thread only (not thread-safe by itself). Produces, inside
// {session_dir}/{camera_id}/:
//   seg_NNNNN.raw        data segments (format.hpp layout)
//   seg_NNNNN.idx.jsonl  one JSON line per frame
//   segments.jsonl       one JSON summary line per closed segment
//
// Crash tolerance: the file header is fdatasync'ed at segment creation, the
// frame header is written before its payload, and the index is strictly a
// subset of the data (payload bytes hit the fd before the index line is
// buffered). rebuild-index recovers header fields, not supplementary SDK layout metadata.

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/uio.h>  // struct iovec (must be declared before the member below)

#include "frame.h"
#include "stats.h"

namespace gox {
    class IoError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct RecorderOptions {
        std::string camera_dir; // {session_dir}/{camera_id}, created by Open()
        std::string camera_id;
        std::string camera_serial;
        uint8_t session_uuid[16] = {};
        uint64_t segment_max_bytes = 2ull << 30;
        uint32_t record_align = 4096;
        uint64_t flush_interval_bytes = 64ull << 20; // sync_file_range cadence
    };

    class Recorder {
    public:
        // stats may be nullptr (unit tests); counters updated: frames_written,
        // bytes_written (record size incl. header+padding), segments_created.
        Recorder(RecorderOptions opts, CameraStats *stats);

        ~Recorder();

        Recorder(const Recorder &) = delete;

        Recorder &operator=(const Recorder &) = delete;

        // Creates camera_dir and the first segment. Throws IoError.
        void Open();

        // Appends one frame record (+ index line). Rotates segments as needed.
        // Throws IoError on write failure. The disk floor is NOT checked here:
        // it is rig-wide and enforced by Main (Guards: in config-main.yaml),
        // which polls it once a second instead of once per 2 GiB segment.
        void WriteFrame(const FrameMeta &meta, const uint8_t *data, size_t size);

        // Flushes and finalizes everything. Idempotent. Throws IoError.
        void close(bool clean = true);

        using IndexWriteHook = std::function<ssize_t(int, const void *, size_t)>;
        void SetIndexWriteHookForTest(IndexWriteHook hook) { index_write_hook_ = std::move(hook); }

        uint64_t FramesWritten() const { return frame_seq_; }
        uint32_t CurrentSegmentIndex() const { return segment_index_; }

    private:
        void OpenSegment();

        void CloseSegment(bool clean);

        void FlushIndex(bool force);

        void AppendSegmentSummary(bool clean) const;

        void WriteIovAll(const struct iovec *iov, int iovcnt, size_t total) const;

        RecorderOptions opts_;
        CameraStats *stats_;

        bool opened_ = false;
        bool closed_ = false;

        int seg_fd_ = -1;
        int idx_fd_ = -1;
        int segments_fd_ = -1;
        int dir_fd_ = -1;

        uint32_t segment_index_ = 0; // 1-based once open
        uint64_t seg_offset_ = 0; // current write offset in the segment
        uint64_t last_synced_off_ = 0;

        uint64_t frame_seq_ = 0; // session-wide, monotonically increasing

        // per-segment bookkeeping for segments.jsonl
        uint64_t seg_frames_ = 0;
        uint64_t seg_seq_first_ = 0, seg_seq_last_ = 0;
        uint64_t seg_bid_first_ = 0, seg_bid_last_ = 0;
        uint64_t seg_dts_first_ = 0, seg_dts_last_ = 0;

        // buffered index lines
        std::string idx_buf_;
        IndexWriteHook index_write_hook_;
        uint64_t idx_committed_bytes_ = 0;
        bool idx_write_failed_ = false;
        uint64_t idx_frames_pending_ = 0;
        uint64_t idx_last_flush_mono_ns_ = 0;
    };
} // namespace gox
