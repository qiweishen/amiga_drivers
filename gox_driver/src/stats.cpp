#include "stats.h"
#include "utility.h"
#include "time_util.h"

#include <cinttypes>
#include <climits>
#include <cstdio>
#include <string>

#include "util.h"

namespace gox {
    void TriggerCounterAccounting::Begin(bool bound, bool reset_ok, std::optional<int64_t> value,
                                         std::optional<bool> overflow) {
        *this = {};
        bound_at_start = bound;
        reset_acknowledged = reset_ok;
        baseline = value;
        Observe(bound, value, overflow);
    }

    void TriggerCounterAccounting::Observe(bool bound, std::optional<int64_t> value,
                                           std::optional<bool> overflow) {
        binding_lost = binding_lost || !bound;
        status_unavailable = status_unavailable || !overflow.has_value();
        overflow_seen = overflow_seen || overflow.value_or(false);
        if (value && (*value < 0 || (previous_valid && *value < *previous_valid))) regressed = true;
        if (value) previous_valid = value;
        latest = value; // a failed final read must not reuse an old periodic sample
    }

    nlohmann::ordered_json TriggerCounterAccounting::FinalJson(uint64_t emitted, bool window_verified) const {
        nlohmann::ordered_json out = {{"bound_at_start", bound_at_start},
            {"reset_acknowledged", reset_acknowledged}, {"binding_lost", binding_lost},
            {"overflow_seen", overflow_seen}, {"counter_regressed", regressed},
            {"status_unavailable", status_unavailable}, {"window_verified", window_verified},
            {"baseline", baseline ? nlohmann::ordered_json(*baseline) : nlohmann::ordered_json(nullptr)},
            {"final_raw", latest ? nlohmann::ordered_json(*latest) : nlohmann::ordered_json(nullptr)},
            {"frames_emitted_accounted", emitted}, {"trigger_delta", nullptr}, {"missing_frames", nullptr},
            {"status", "unknown"}, {"unknown_reason", nullptr}, {"failed", false}, {"association_verified", false},
            {"semantics", "Counter0 delta versus emitted-frame accounting after drain; no frame/trigger anchor"}};
        std::string unknown;
        if (!bound_at_start || binding_lost) unknown = "Counter0 FrameTrigger binding unavailable at one or more sampled reads";
        else if (!reset_acknowledged) unknown = "CounterReset not acknowledged";
        else if (overflow_seen || regressed) unknown = "counter overflow or regression invalidated the delta";
        else if (status_unavailable) unknown = "counter overflow status unavailable";
        else if (!baseline || !latest || *baseline < 0 || *latest < *baseline) unknown = "baseline or final counter reading unavailable/invalid";
        else if (!window_verified) unknown = "acquisition counter window not bounded by the shared SensorSync source";
        if (!unknown.empty()) { out["unknown_reason"] = unknown; return out; }
        const auto delta = static_cast<uint64_t>(*latest - *baseline);
        out["trigger_delta"] = delta;
        out["status"] = delta == emitted ? "balanced" : delta > emitted ? "missing_frames" : "counter_disagreement";
        out["failed"] = delta != emitted;
        if (delta >= emitted) out["missing_frames"] = delta - emitted;
        return out;
    }

    nlohmann::ordered_json CameraStatsJson(const CameraStats::Snapshot &s) {
        return {{"frames_retrieved_ok", s.frames_retrieved_ok}, {"frames_incomplete", s.frames_incomplete},
            {"frames_error_dropped", s.frames_error_dropped}, {"frames_dropped_queue", s.frames_dropped_queue},
            {"blockid_gap_events", s.blockid_gap_events}, {"frames_lost_gap", s.frames_lost_gap},
            {"frames_limit_excluded", s.frames_limit_excluded},
            {"frames_written", s.frames_written}, {"bytes_written", s.bytes_written},
            {"frames_write_unconfirmed", s.frames_write_unconfirmed},
            {"unconfirmed_semantics", "failed or unattempted writes; may overlap frames_written after a durability failure; not additive loss"},
            {"segments_created", s.segments_created}, {"stream_blocks_dropped", s.stream_blocks_dropped},
            {"stream_error_count", s.stream_error_count}, {"queue_depth", s.queue_depth},
            {"queue_capacity", s.queue_capacity},
            {"sensor_temp_centi", s.sensor_temp_centi != INT32_MIN ? nlohmann::ordered_json(s.sensor_temp_centi) : nlohmann::ordered_json(nullptr)},
            {"trigger_count_raw", s.trigger_count >= 0 ? nlohmann::ordered_json(s.trigger_count) : nlohmann::ordered_json(nullptr)},
            {"trigger_overflow", s.trigger_overflow},
            {"device_gauges_semantics", "last available sample; use trigger_counter for final-read availability and validated delta"}};
    }

    CameraStats::Snapshot CameraStats::GetSnapshot() const {
        Snapshot s{};
        s.frames_retrieved_ok = frames_retrieved_ok.load(std::memory_order_relaxed);
        s.frames_incomplete = frames_incomplete.load(std::memory_order_relaxed);
        s.frames_error_dropped = frames_error_dropped.load(std::memory_order_relaxed);
        s.frames_dropped_queue = frames_dropped_queue.load(std::memory_order_relaxed);
        s.blockid_gap_events = blockid_gap_events.load(std::memory_order_relaxed);
        s.frames_lost_gap = frames_lost_gap.load(std::memory_order_relaxed);
        s.frames_limit_excluded = frames_limit_excluded.load(std::memory_order_relaxed);
        s.frames_written = frames_written.load(std::memory_order_relaxed);
        s.frames_write_unconfirmed = frames_write_unconfirmed.load(std::memory_order_relaxed);
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
                                         cur.frames_error_dropped + cur.frames_dropped_queue + cur.frames_lost_gap +
                                         cur.frames_limit_excluded;
            const uint64_t emitted_prev = prev_.frames_retrieved_ok + prev_.frames_incomplete +
                                          prev_.frames_error_dropped + prev_.frames_dropped_queue +
                                          prev_.frames_lost_gap + prev_.frames_limit_excluded;
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
            // The raw value alone has no verified baseline/window. Session
            // final statistics carry the separately validated reconciliation.
            snprintf(buf, sizeof(buf), "  triggers=%" PRId64 "  missed=unknown", s.trigger_count);
            line += buf;
        }
        return line;
    }
} // namespace gox
