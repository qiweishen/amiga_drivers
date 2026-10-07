#pragma once

// Per-camera counters, written from the acquisition/writer threads with
// relaxed atomics and read from the main thread for the periodic status line
// and the final summary.

#include <atomic>
#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace gox {
    // Owner-thread evidence; inspect only after acquisition has drained. A raw
    // Counter0 value is not a frame/trigger anchor or an acquisition-window delta.
    struct TriggerCounterAccounting {
        bool bound_at_start = false, reset_acknowledged = false;
        bool binding_lost = false, overflow_seen = false, regressed = false;
        bool status_unavailable = false;
        std::optional<int64_t> baseline, latest, previous_valid;

        void Begin(bool bound, bool reset_ok, std::optional<int64_t> value,
                   std::optional<bool> overflow);
        void Observe(bool bound, std::optional<int64_t> value, std::optional<bool> overflow);
        // window_verified requires our stopped SensorSync pulse source. Counts
        // collected against an independently controlled source remain unknown.
        nlohmann::ordered_json FinalJson(uint64_t emitted, bool window_verified) const;
    };

    struct CameraStats {
        // acquisition side
        std::atomic<uint64_t> frames_retrieved_ok{0};
        std::atomic<uint64_t> frames_incomplete{0}; // recorded with INCOMPLETE flag
        std::atomic<uint64_t> frames_error_dropped{0}; // on_buffer_error=drop
        std::atomic<uint64_t> frames_dropped_queue{0}; // pool/queue full (drop_newest)
        std::atomic<uint64_t> blockid_gap_events{0}; // number of gap occurrences
        std::atomic<uint64_t> frames_lost_gap{0}; // sum of missing BlockIDs
        std::atomic<uint64_t> frames_limit_excluded{0}; // received after the explicit max_frames limit
        // writer side
        std::atomic<uint64_t> frames_written{0};
        std::atomic<uint64_t> frames_write_unconfirmed{0}; // failed/unattempted writes; may overlap frames_written
        std::atomic<uint64_t> bytes_written{0};
        std::atomic<uint64_t> segments_created{0};
        // stream-layer stats mirrored from PvStream parameters (main thread poll)
        std::atomic<uint64_t> stream_blocks_dropped{0};
        std::atomic<uint64_t> stream_error_count{0};
        // gauges
        std::atomic<uint64_t> queue_depth{0};
        std::atomic<uint64_t> queue_capacity{0};
        // device telemetry from the monitor thread's poll; sentinels = never read
        std::atomic<int32_t> sensor_temp_centi{INT32_MIN}; // DeviceTemperature[Sensor] x100
        std::atomic<int64_t> trigger_count{-1}; // CounterValue[Counter0], -1 = not bound
        std::atomic<bool> trigger_overflow{false}; // CounterStatus == CounterOverflow
        // Main's no-data watchdog reference: CLOCK_MONOTONIC ns of the last buffer (or of
        // AcquisitionStart before the first); 0 = not acquiring
        std::atomic<uint64_t> data_reference_mono_ns{0};

        struct Snapshot {
            uint64_t frames_retrieved_ok, frames_incomplete, frames_error_dropped, frames_dropped_queue;
            uint64_t blockid_gap_events, frames_lost_gap, frames_limit_excluded;
            uint64_t frames_written, frames_write_unconfirmed, bytes_written, segments_created;
            uint64_t stream_blocks_dropped, stream_error_count;
            uint64_t queue_depth, queue_capacity;
            int32_t sensor_temp_centi;
            int64_t trigger_count;
            bool trigger_overflow;
        };

        Snapshot GetSnapshot() const;
    };

    nlohmann::ordered_json CameraStatsJson(const CameraStats::Snapshot &stats);

    // "[Statistics] [cam0] up=  rate= Hz  fps=  ok=  incomp=  drop_q=  drop_net=  gaps=  q=  seg=  written=  temp=  trig="
    // GUI contract: keys are only appended (tools/check_contracts.py pins fps=). rate = sensor output
    // incl. dropped/lost frames, fps = frames written; both against the previous snapshot
    class StatsReporter {
    public:
        StatsReporter(std::string camera_id, const CameraStats *stats);

        // interval_s: seconds since the previous call (rate basis)
        std::string PeriodicLine(double interval_s, uint64_t uptime_s);

        std::string FinalSummary(uint64_t uptime_s) const;

    private:
        std::string camera_id_;
        const CameraStats *stats_;
        CameraStats::Snapshot prev_{};
    };
} // namespace gox
