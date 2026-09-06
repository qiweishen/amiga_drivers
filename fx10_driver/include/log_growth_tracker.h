#pragma once

#include <chrono>
#include <cstdint>

// Stall detection for an append-only log file (the SensorSync timing log).
//
// The decision is "has the file GROWN recently", measured against a STEADY
// clock. The obvious alternative — mtime against the wall clock — is wrong
// twice over: mtime has 1 s granularity, and any wall-clock jump (an NTP step
// on a rig that just acquired GNSS time is routine) fabricates an arbitrary
// stall and aborts a perfectly healthy recording.
//
// Header-only and clock-injected so tests can drive it without a filesystem.

namespace fx10 {
    class LogGrowthTracker {
    public:
        using Clock = std::chrono::steady_clock;

        // Baseline at session start. Call even when the size is unknown (-1):
        // leaving `now` at the steady_clock epoch would report an instant stall.
        void Reset(std::int64_t size, Clock::time_point now) {
            last_size_ = size;
            last_growth_ = now;
        }

        // Seconds since the file last changed size; 0.0 when it changed now.
        // A SHRINKING file counts as activity too — the tracker only cares that
        // something is still writing.
        double Update(std::int64_t size, Clock::time_point now) {
            if (size != last_size_) {
                last_size_ = size;
                last_growth_ = now;
                return 0.0;
            }
            return std::chrono::duration<double>(now - last_growth_).count();
        }

        std::int64_t LastSize() const { return last_size_; }

    private:
        std::int64_t last_size_ = -1; // -1 = not sampled yet
        Clock::time_point last_growth_{};
    };
} // namespace fx10
