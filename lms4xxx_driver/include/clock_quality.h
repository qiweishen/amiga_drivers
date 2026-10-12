#pragma once
#include <cstdint>

namespace lms4xxx {
    namespace ClockFlag {
        inline constexpr std::uint16_t kAssessed = 0x001;
        inline constexpr std::uint16_t kInvalidTimestamp = 0x002;
        inline constexpr std::uint16_t kFirstSample = 0x004;
        inline constexpr std::uint16_t kRepeatedUtc = 0x008;
        inline constexpr std::uint16_t kBackwardUtc = 0x010;
        inline constexpr std::uint16_t kStepExceeded = 0x020;
        inline constexpr std::uint16_t kNtpDisabled = 0x040;
        inline constexpr std::uint16_t kDeviceNoNtp = 0x080;
        inline constexpr std::uint16_t kDeviceNtpUnknown = 0x100;
        inline constexpr std::uint16_t kUptimeDiscontinuity = 0x200;
        // Neither a plausible date nor a clear warning list proves absolute accuracy.
        inline constexpr std::uint16_t kAbsoluteTimeUnverified = 0x400;
    }

    struct ClockObservation {
        std::uint16_t flags = 0; // 0 = not assessed (e.g. a parser-only consumer)
        std::int64_t step_us = 0; // UTC delta minus unsigned device uptime delta; only valid after first sample
        // Diagnostics between consecutive plausible timestamps; zero on a
        // first/invalid sample. Preserve UTC offset even if uptime is unusable.
        std::int64_t utc_delta_us = 0; // current UTC minus previous UTC (negative = regression)
        std::uint32_t uptime_delta_us = 0; // raw modulo-2^32 delta; inspect kUptimeDiscontinuity
    };

    // Diagnostic logging only; independent of the configured time-fault limit.
    inline constexpr std::int64_t kClockStepLogThresholdUs = 1000;
    // TIME-ANOMALY uses strict limits, independently of logging and fault stops.
    inline constexpr std::int64_t kUtcRegressionAnomalyThresholdUs = 1000;
    inline constexpr std::int64_t kClockStepAnomalyThresholdUs = 10000;

    inline bool IsTimeAnomaly(const ClockObservation &observation) {
        if (!(observation.flags & ClockFlag::kAssessed) ||
            (observation.flags & (ClockFlag::kFirstSample | ClockFlag::kInvalidTimestamp | ClockFlag::kNtpDisabled))) {
            return false;
        }
        // The UTC delta remains meaningful even if uptime is discontinuous.
        if (observation.utc_delta_us < -kUtcRegressionAnomalyThresholdUs) return true;
        if (observation.flags & ClockFlag::kUptimeDiscontinuity) return false;
        return observation.step_us > kClockStepAnomalyThresholdUs ||
               observation.step_us < -kClockStepAnomalyThresholdUs;
    }

    inline bool ShouldLogClockChange(const ClockObservation &observation) {
        if (!(observation.flags & ClockFlag::kAssessed) ||
            (observation.flags & (ClockFlag::kFirstSample | ClockFlag::kInvalidTimestamp | ClockFlag::kNtpDisabled))) {
            return false;
        }
        // Every actual UTC regression is observable even if uptime reset.
        if (observation.flags & ClockFlag::kBackwardUtc) return true;
        if (observation.flags & ClockFlag::kUptimeDiscontinuity) return false;
        return observation.step_us >= kClockStepLogThresholdUs ||
               observation.step_us <= -kClockStepLogThresholdUs;
    }

    // Parse-thread owned. No clock correction, sorting, or fabricated samples.
    class ClockQualityTracker {
    public:
        ClockObservation Observe(std::int64_t utc, bool plausible, std::uint32_t uptime,
                                 std::uint32_t max_step_ms) {
            ClockObservation out{ClockFlag::kAssessed | ClockFlag::kAbsoluteTimeUnverified, 0};
            if (!plausible) {
                out.flags |= ClockFlag::kInvalidTimestamp;
                previous_valid_ = false;
                return out;
            }
            if (!previous_valid_) out.flags |= ClockFlag::kFirstSample;
            else {
                const std::uint32_t du = uptime - previous_uptime_;
                const auto dt = utc - previous_utc_;
                out.utc_delta_us = dt;
                out.uptime_delta_us = du;
                if (dt < 0) out.flags |= ClockFlag::kBackwardUtc;
                if (dt == 0) out.flags |= ClockFlag::kRepeatedUtc;
                // A normal rollover gives a small positive unsigned delta. A
                // reset/backward uptime or >35-minute gap cannot be unwrapped
                // reliably from consecutive live scans.
                if (du == 0 || du > 0x7fffffffu) out.flags |= ClockFlag::kUptimeDiscontinuity;
                else {
                    out.step_us = dt - static_cast<std::int64_t>(du);
                    const auto magnitude = out.step_us < 0 ? -out.step_us : out.step_us;
                    if (magnitude > static_cast<std::int64_t>(max_step_ms) * 1000)
                        out.flags |= ClockFlag::kStepExceeded;
                }
            }
            previous_valid_ = true;
            previous_utc_ = utc;
            previous_uptime_ = uptime;
            return out;
        }
    private:
        bool previous_valid_ = false;
        std::int64_t previous_utc_ = 0;
        std::uint32_t previous_uptime_ = 0;
    };
} // namespace lms4xxx
