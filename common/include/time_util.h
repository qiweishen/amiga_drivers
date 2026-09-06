#pragma once

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <string>


namespace common::TimeUtil {
    // Wall clock (CLOCK_REALTIME), nanoseconds since the Unix epoch (UTC)
    inline std::uint64_t RealtimeNowNs() {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
    }

    // Monotonic clock (CLOCK_MONOTONIC), nanoseconds since an arbitrary origin
    inline std::uint64_t MonotonicNowNs() {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
    }

    // std::chrono::steady_clock now, microseconds since its epoch
    inline std::uint64_t SteadyNowUs() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    }

    // Wall clock (CLOCK_REALTIME), microseconds since the Unix epoch
    inline std::uint64_t RealtimeNowUs() {
        return RealtimeNowNs() / 1000ull;
    }

    // "2026-07-17T11:54:00Z" from a CLOCK_REALTIME timestamp; the FX10 ENVI .hdr freezes this exact format
    inline std::string Iso8601UtcSec(std::uint64_t realtime_ns) {
        const auto secs = static_cast<time_t>(realtime_ns / 1000000000ull);
        tm tm_utc{};
        gmtime_r(&secs, &tm_utc);
        char buf[72]; // generous: silences -Wformat-truncation for absurd tm_year values
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                 tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
        return buf;
    }

    // Compact UTC stamp usable in file names: "20260717T115400Z"
    inline std::string CompactUtc(std::uint64_t realtime_ns) {
        const auto secs = static_cast<time_t>(realtime_ns / 1000000000ull);
        tm tm_utc{};
        gmtime_r(&secs, &tm_utc);
        char buf[72]; // generous: silences -Wformat-truncation for absurd tm_year values
        snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                 tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
        return buf;
    }

    inline std::string CompactUtcNow() {
        return CompactUtc(RealtimeNowNs());
    }

    // "HH:MM:SS" from a duration in seconds
    inline std::string HumanDuration(std::uint64_t seconds) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, seconds / 3600, (seconds % 3600) / 60,
                 seconds % 60);
        return buf;
    }
} // namespace common::TimeUtil
