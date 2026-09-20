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
    };

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
