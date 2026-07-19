#pragma once

// Per-camera counters, written from the acquisition/writer threads with
// relaxed atomics and read from the main thread for the periodic status line
// and the final summary.

#include <atomic>
#include <cstdint>
#include <string>

namespace jai {

	struct CameraStats {
		// acquisition side
		std::atomic<uint64_t> frames_retrieved_ok{ 0 };
		std::atomic<uint64_t> frames_incomplete{ 0 };	  // recorded with INCOMPLETE flag
		std::atomic<uint64_t> frames_error_dropped{ 0 };  // on_buffer_error=drop
		std::atomic<uint64_t> frames_dropped_queue{ 0 };  // pool/queue full (drop_newest)
		std::atomic<uint64_t> blockid_gap_events{ 0 };	  // number of gap occurrences
		std::atomic<uint64_t> frames_lost_gap{ 0 };		  // sum of missing BlockIDs
		std::atomic<uint64_t> retrieve_timeouts{ 0 };
		// writer side
		std::atomic<uint64_t> frames_written{ 0 };
		std::atomic<uint64_t> bytes_written{ 0 };
		std::atomic<uint64_t> segments_created{ 0 };
		// stream-layer stats mirrored from PvStream parameters (main thread poll)
		std::atomic<uint64_t> stream_blocks_dropped{ 0 };
		std::atomic<uint64_t> stream_error_count{ 0 };
		// gauges
		std::atomic<uint64_t> queue_depth{ 0 };
		std::atomic<uint64_t> queue_capacity{ 0 };

		struct Snapshot {
			uint64_t frames_retrieved_ok, frames_incomplete, frames_error_dropped, frames_dropped_queue;
			uint64_t blockid_gap_events, frames_lost_gap, retrieve_timeouts;
			uint64_t frames_written, bytes_written, segments_created;
			uint64_t stream_blocks_dropped, stream_error_count;
			uint64_t queue_depth, queue_capacity;
		};
		Snapshot snapshot() const;
	};

	// One periodic status line, e.g.
	// [cam0] up=00:01:05 fps=24.0 disk=119.8MB/s ok=1560 incomp=2 drop_q=0 drop_net=0 q=3/32 seg=4 free=812GiB
	// fps/disk rates are computed against the previous snapshot.
	class StatsReporter {
	public:
		StatsReporter(std::string camera_id, const CameraStats *stats);

		// interval_s: seconds since the previous call (rate basis).
		std::string periodic_line(double interval_s, uint64_t uptime_s, uint64_t free_disk_bytes);

		std::string final_summary(uint64_t uptime_s) const;

	private:
		std::string camera_id_;
		const CameraStats *stats_;
		CameraStats::Snapshot prev_{};
	};

}  // namespace jai
