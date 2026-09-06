#include "stats.h"
#include "utility.h"
#include "time_util.h"

#include <cinttypes>
#include <climits>
#include <cstdio>
#include <string>

#include "util.h"

namespace gox {
    CameraStats::Snapshot CameraStats::GetSnapshot() const {
        Snapshot s{};
        s.frames_retrieved_ok = frames_retrieved_ok.load(std::memory_order_relaxed);
        s.frames_incomplete = frames_incomplete.load(std::memory_order_relaxed);
        s.frames_error_dropped = frames_error_dropped.load(std::memory_order_relaxed);
        s.frames_dropped_queue = frames_dropped_queue.load(std::memory_order_relaxed);
        s.blockid_gap_events = blockid_gap_events.load(std::memory_order_relaxed);
        s.frames_lost_gap = frames_lost_gap.load(std::memory_order_relaxed);
        s.frames_written = frames_written.load(std::memory_order_relaxed);
        s.bytes_written = bytes_written.load(std::memory_order_relaxed);
        s.segments_created = segments_created.load(std::memory_order_relaxed);
        s.stream_blocks_dropped = stream_blocks_dropped.load(std::memory_order_relaxed);
        s.stream_error_count = stream_error_count.load(std::memory_order_relaxed);
        s.queue_depth = queue_depth.load(std::memory_order_relaxed);
        s.queue_capacity = queue_capacity.load(std::memory_order_relaxed);
        s.sensor_temp_centi = sensor_temp_centi.load(std::memory_order_relaxed);
        s.trigger_count = trigger_count.load(std::memory_order_relaxed);
        s.trigger_overflow = trigger_overflow.load(std::memory_order_relaxed);
        return s;
    }

    StatsReporter::StatsReporter(std::string camera_id, const CameraStats *stats) : camera_id_(std::move(camera_id)),
        stats_(stats) {
    }

    std::string StatsReporter::PeriodicLine(double interval_s, uint64_t uptime_s) {
        CameraStats::Snapshot cur = stats_->GetSnapshot();
        double rate_hz = 0.0;
        double fps = 0.0;
        double mbps = 0.0;
        if (interval_s > 0.0) {
            // Sensor's ACTUAL frame rate: every frame the camera emitted in the
            // interval, whether it was written, degraded, dropped on a full
            // queue, or lost on the wire (each buffer lands in exactly one of
            // these counters; lost_gap counts the ones that never arrived).
            const uint64_t emitted_cur = cur.frames_retrieved_ok + cur.frames_incomplete +
                                         cur.frames_error_dropped + cur.frames_dropped_queue + cur.frames_lost_gap;
            const uint64_t emitted_prev = prev_.frames_retrieved_ok + prev_.frames_incomplete +
                                          prev_.frames_error_dropped + prev_.frames_dropped_queue +
                                          prev_.frames_lost_gap;
            rate_hz = static_cast<double>(emitted_cur - emitted_prev) / interval_s;
            fps = static_cast<double>(cur.frames_written - prev_.frames_written) / interval_s;
            mbps = static_cast<double>(cur.bytes_written - prev_.bytes_written) / interval_s / 1e6;
        }
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "[Statistics] [%s] up=%s  rate=%.1f Hz  fps=%.1f  disk=%.1f MB/s  ok=%" PRIu64 "  incomp=%" PRIu64
                 "  drop_q=%" PRIu64 "  drop_net=%" PRIu64
                 "  gaps=%" PRIu64 "(-%" PRIu64 ")  q=%" PRIu64 "/%" PRIu64 "  seg=%" PRIu64 "  written=%s",
                 camera_id_.c_str(), common::TimeUtil::HumanDuration(uptime_s).c_str(), rate_hz, fps, mbps, cur.frames_retrieved_ok,
                 cur.frames_incomplete,
                 cur.frames_dropped_queue, cur.stream_blocks_dropped, cur.blockid_gap_events, cur.frames_lost_gap,
                 cur.queue_depth,
                 cur.queue_capacity, cur.segments_created, common::HumanBytes(cur.bytes_written).c_str());
        // The format string above is pinned by tools/check_contracts.py and
        // parsed by app/services/driver_stats.py; new keys are appended, never
        // interleaved. No unit and no inner space: the GUI splits fields on the
        // double space.
        std::string line = buf;
        if (cur.sensor_temp_centi != INT32_MIN) {
            snprintf(buf, sizeof(buf), "  temp=%.1f", static_cast<double>(cur.sensor_temp_centi) / 100.0);
            line += buf;
        }
        if (cur.trigger_count >= 0) {
            snprintf(buf, sizeof(buf), "  trig=%" PRId64 "%s", cur.trigger_count,
                     cur.trigger_overflow ? "(ovf)" : "");
            line += buf;
        }
        prev_ = cur;
        return line;
    }

    std::string StatsReporter::FinalSummary(uint64_t uptime_s) const {
        CameraStats::Snapshot s = stats_->GetSnapshot();
        char buf[640];
        snprintf(buf, sizeof(buf),
                 "[Statistics] [%s] Final: duration=%s  frames_ok=%" PRIu64 "  incomplete=%" PRIu64
                 "  dropped_queue=%" PRIu64
                 "  dropped_error=%" PRIu64 "  blockid_gaps=%" PRIu64 "  frames_lost=%" PRIu64
                 "  stream_blocks_dropped=%" PRIu64
                 "  stream_errors=%" PRIu64 "  frames_written=%" PRIu64 "  bytes=%s  segments=%" PRIu64,
                 camera_id_.c_str(), common::TimeUtil::HumanDuration(uptime_s).c_str(), s.frames_retrieved_ok, s.frames_incomplete,
                 s.frames_dropped_queue, s.frames_error_dropped, s.blockid_gap_events, s.frames_lost_gap,
                 s.stream_blocks_dropped,
                 s.stream_error_count, s.frames_written, common::HumanBytes(s.bytes_written).c_str(), s.segments_created);
        std::string line = buf;
        if (s.trigger_count >= 0) {
            // Triggers the camera received (Counter0) minus the frames it
            // actually emitted: the only camera-side evidence of a trigger that
            // was masked or arrived while the camera could not answer it.
            // Signed on purpose - a negative value means the counter and the
            // frame stream disagree and the number should not be trusted.
            const int64_t emitted = static_cast<int64_t>(s.frames_retrieved_ok + s.frames_incomplete +
                                                         s.frames_error_dropped + s.frames_dropped_queue +
                                                         s.frames_lost_gap);
            snprintf(buf, sizeof(buf), "  triggers=%" PRId64 "  missed=%" PRId64, s.trigger_count,
                     s.trigger_count - emitted);
            line += buf;
        }
        return line;
    }
} // namespace gox
