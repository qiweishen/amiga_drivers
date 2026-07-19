#include "core/stats.hpp"

#include <cinttypes>
#include <cstdio>

#include "core/util.hpp"

namespace jai {

	CameraStats::Snapshot CameraStats::snapshot() const {
		Snapshot s{};
		s.frames_retrieved_ok = frames_retrieved_ok.load(std::memory_order_relaxed);
		s.frames_incomplete = frames_incomplete.load(std::memory_order_relaxed);
		s.frames_error_dropped = frames_error_dropped.load(std::memory_order_relaxed);
		s.frames_dropped_queue = frames_dropped_queue.load(std::memory_order_relaxed);
		s.blockid_gap_events = blockid_gap_events.load(std::memory_order_relaxed);
		s.frames_lost_gap = frames_lost_gap.load(std::memory_order_relaxed);
		s.retrieve_timeouts = retrieve_timeouts.load(std::memory_order_relaxed);
		s.frames_written = frames_written.load(std::memory_order_relaxed);
		s.bytes_written = bytes_written.load(std::memory_order_relaxed);
		s.segments_created = segments_created.load(std::memory_order_relaxed);
		s.stream_blocks_dropped = stream_blocks_dropped.load(std::memory_order_relaxed);
		s.stream_error_count = stream_error_count.load(std::memory_order_relaxed);
		s.queue_depth = queue_depth.load(std::memory_order_relaxed);
		s.queue_capacity = queue_capacity.load(std::memory_order_relaxed);
		return s;
	}

	StatsReporter::StatsReporter(std::string camera_id, const CameraStats *stats) : camera_id_(std::move(camera_id)), stats_(stats) {}

	std::string StatsReporter::periodic_line(double interval_s, uint64_t uptime_s, uint64_t free_disk_bytes) {
		CameraStats::Snapshot cur = stats_->snapshot();
		double fps = 0.0;
		double mbps = 0.0;
		if (interval_s > 0.0) {
			fps = static_cast<double>(cur.frames_written - prev_.frames_written) / interval_s;
			mbps = static_cast<double>(cur.bytes_written - prev_.bytes_written) / interval_s / 1e6;
		}
		char buf[512];
		snprintf(buf, sizeof(buf),
				 "[%s] up=%s fps=%.1f disk=%.1fMB/s ok=%" PRIu64 " incomp=%" PRIu64 " drop_q=%" PRIu64 " drop_net=%" PRIu64
				 " gaps=%" PRIu64 "(-%" PRIu64 ") q=%" PRIu64 "/%" PRIu64 " seg=%" PRIu64 " written=%s free=%s",
				 camera_id_.c_str(), human_duration(uptime_s).c_str(), fps, mbps, cur.frames_retrieved_ok, cur.frames_incomplete,
				 cur.frames_dropped_queue, cur.stream_blocks_dropped, cur.blockid_gap_events, cur.frames_lost_gap, cur.queue_depth,
				 cur.queue_capacity, cur.segments_created, human_bytes(cur.bytes_written).c_str(),
				 human_bytes(free_disk_bytes).c_str());
		prev_ = cur;
		return buf;
	}

	std::string StatsReporter::final_summary(uint64_t uptime_s) const {
		CameraStats::Snapshot s = stats_->snapshot();
		char buf[640];
		snprintf(buf, sizeof(buf),
				 "[%s] session summary: duration=%s frames_ok=%" PRIu64 " incomplete=%" PRIu64 " dropped_queue=%" PRIu64
				 " dropped_error=%" PRIu64 " blockid_gaps=%" PRIu64 " frames_lost=%" PRIu64 " stream_blocks_dropped=%" PRIu64
				 " stream_errors=%" PRIu64 " frames_written=%" PRIu64 " bytes=%s segments=%" PRIu64,
				 camera_id_.c_str(), human_duration(uptime_s).c_str(), s.frames_retrieved_ok, s.frames_incomplete,
				 s.frames_dropped_queue, s.frames_error_dropped, s.blockid_gap_events, s.frames_lost_gap, s.stream_blocks_dropped,
				 s.stream_error_count, s.frames_written, human_bytes(s.bytes_written).c_str(), s.segments_created);
		return buf;
	}

}  // namespace jai
