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
// buffered). scripts/inspect_raw.py rebuild-index recovers everything else.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <sys/uio.h>  // struct iovec (must be declared before the member below)

#include "core/frame.hpp"
#include "core/stats.hpp"

namespace jai {

	class IoError : public std::runtime_error {
	public:
		using std::runtime_error::runtime_error;
	};

	struct RecorderOptions {
		std::string camera_dir;	 // {session_dir}/{camera_id}, created by open()
		std::string camera_id;
		std::string camera_serial;
		uint8_t session_uuid[16] = {};
		uint64_t segment_max_bytes = 2ull << 30;
		uint32_t record_align = 4096;
		bool payload_crc = false;
		uint64_t flush_interval_bytes = 64ull << 20;  // sync_file_range cadence
		uint64_t min_free_bytes = 0;				  // checked at rotation; 0 = off
		uint32_t debug_slowdown_us = 0;				  // test hook (writer sleep per frame)
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
		void open();

		// Appends one frame record (+ index line). Rotates segments as needed.
		// Throws IoError on write failure or when free disk space drops below
		// min_free_bytes at a rotation boundary.
		void write_frame(const FrameMeta &meta, const uint8_t *data, size_t size);

		// Flushes and finalizes everything. Idempotent. Throws IoError.
		void close();

		uint64_t frames_written() const { return frame_seq_; }
		uint32_t current_segment_index() const { return segment_index_; }

	private:
		void open_segment();
		void close_segment(bool clean);
		void flush_index(bool force);
		void append_segment_summary(bool clean);
		void check_free_space();
		void write_iov_all(const struct iovec *iov, int iovcnt, size_t total);

		RecorderOptions opts_;
		CameraStats *stats_;

		bool opened_ = false;
		bool closed_ = false;

		int seg_fd_ = -1;
		int idx_fd_ = -1;
		int segments_fd_ = -1;
		int dir_fd_ = -1;

		uint32_t segment_index_ = 0;  // 1-based once open
		uint64_t seg_offset_ = 0;	  // current write offset in the segment
		uint64_t last_synced_off_ = 0;

		uint64_t frame_seq_ = 0;	  // session-wide, monotonically increasing

		// per-segment bookkeeping for segments.jsonl
		uint64_t seg_frames_ = 0;
		uint64_t seg_seq_first_ = 0, seg_seq_last_ = 0;
		uint64_t seg_bid_first_ = 0, seg_bid_last_ = 0;
		uint64_t seg_dts_first_ = 0, seg_dts_last_ = 0;

		// buffered index lines
		std::string idx_buf_;
		uint64_t idx_frames_pending_ = 0;
		uint64_t idx_last_flush_mono_ns_ = 0;
	};

}  // namespace jai
