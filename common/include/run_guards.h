#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>


namespace common {
    // "When is this recording no longer worth continuing?" — one policy for the
    // whole rig, evaluated by the unified main rather than re-invented per
    // driver. Two independent guards:
    //   * disk    — one filesystem, because every driver writes under
    //               <output>/<timestamp>/raw/;
    //   * no-data — per sensor, because a single dead sensor already makes the
    //               session incomplete.
    struct GuardConfig {
        double disk_min_free_gib = 5.0; // hard floor: stop the whole Run (0 = off)
        double disk_warn_free_gib = 20.0; // soft floor: WARN once per crossing (0 = off)
        double no_data_warn_s = 5.0; // one sensor silent this long -> WARN (0 = off)
        // Must stay ABOVE the AsteRx session's own 30 s SBF watchdog, which
        // triggers a RECONNECT: at 30 s the two race, and a recoverable link
        // blip would end the whole run instead of being repaired.
        double no_data_abort_s = 60.0; // one sensor silent this long -> stop the Run (0 = off)
    };


    // Space available to an unprivileged writer (statvfs f_bavail, i.e. the
    // root-reserved blocks are already excluded), in GiB; -1 when it cannot be
    // read. GiB and f_bavail on purpose: it is what the GUI's disk gauge shows
    // (shutil.disk_usage().free), so a threshold means the same number in both.
    double FreeSpaceGiB(const std::filesystem::path &path);


    // Pure decision layer: no filesystem, no clock, no logging. Everything that
    // decides whether a run continues is therefore reachable from a unit test.
    class RunGuards {
    public:
        enum class Verdict {
            kOk, // nothing to say
            kWarn, // log it, keep recording
            kStop // give up on this run
        };

        struct Result {
            Verdict verdict = Verdict::kOk;
            std::string message; // empty iff verdict == kOk
        };

        explicit RunGuards(GuardConfig cfg) : cfg_(cfg) {
        }

        const GuardConfig &config() const { return cfg_; }

        // free_gib < 0 means "could not be read": never a stop verdict, because
        // an unreadable statvfs is not evidence that the disk is full.
        //
        // The warning latches: below the soft floor it is reported once and
        // re-armed only after the free space climbs back above it. Without that,
        // a run parked between the two floors emits one warning per tick for
        // hours.
        Result CheckDisk(double free_gib);

        // silent_us == nullopt means the driver considers its own silence
        // expected right now (external trigger idle, warming up, not streaming
        // yet) — the watchdog does not apply and any latched warning is dropped.
        //
        // `name` identifies the sensor for the warning latch ONLY; the returned
        // message is a bare predicate ("silent for 31.0 s, ...") that the caller
        // prefixes with the sensor's display name, because that prefix is also
        // the GUI's routing key (markers.DRIVER_NAME_TO_SENSOR_KEY).
        Result CheckSensor(std::string_view name, std::optional<std::uint64_t> silent_us);

    private:
        GuardConfig cfg_;
        bool disk_warn_latched_ = false;
        // Heterogeneous lookup: the caller's key is a string_view into a driver
        // slot name, so a per-tick std::string is not needed to query.
        std::map<std::string, bool, std::less<> > sensor_warn_latched_;
    };
} // namespace common
