#pragma once

// Capture orchestration driven by the AmigaDrivers wrapper (GoxDriverApp).
// Owns the session directory, the per-camera CameraSessions, and the
// periodic stats/limits/PTP loop. The injected StopController is driven
// from an external terminate flag via monitor_loop()'s predicate.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_config.hpp"
#include "signal_stop.hpp"


namespace jai::ebus {
    class CameraSession;
}

namespace jai {
    class CaptureRunner {
    public:
        // stop must outlive the runner (CameraSessions keep the raw pointer).
        CaptureRunner(AppConfig cfg, StopController *stop);

        ~CaptureRunner();

        CaptureRunner(const CaptureRunner &) = delete;

        CaptureRunner &operator=(const CaptureRunner &) = delete;

        // Session dir + camera bring-up. session_dir_override: empty ->
        // output.output_dir/<utc-stamp>_<uuid-prefix>; non-empty -> used
        // verbatim, name = basename. Never throws for control flow: on failure
        // the partially started sessions are torn down, the concrete error is
        // logged and kept in last_error().
        [[nodiscard]] bool init(const std::string &session_dir_override);

        // Blocks in the 200 ms stop-condition poll loop (max_duration_s,
        // periodic stats, PTP refresh) — fx10's monitorLoop_. external_stop
        // (optional) is polled every iteration; true requests
        // StopReason::External. Returns immediately if init() failed.
        void monitor_loop(const std::function<bool()> &external_stop = {});

        // stop_and_join all sessions. Idempotent. Returns true when the
        // session ended cleanly (no error stop and zero drops/gaps); the
        // concrete cause of an unclean end is logged and kept in last_error().
        bool shutdown();

        // Concrete message of the last failure ("" when everything is clean).
        const std::string &last_error() const { return last_error_; }
        bool stop_requested() const { return stop_->stop_requested(); }
        StopReason stop_reason() const { return stop_->reason(); }

    private:
        AppConfig cfg_;
        StopController *stop_;
        std::string session_dir_;
        std::string session_name_;
        uint8_t session_uuid_[16] = {};
        uint64_t start_rt_ = 0;
        uint64_t capture_start_mono_ = 0;
        std::vector<std::unique_ptr<ebus::CameraSession> > sessions_;
        std::string last_error_;
        bool initialized_ = false;
        bool shutdown_done_ = false;
        bool clean_ = true;
    };
} // namespace jai
