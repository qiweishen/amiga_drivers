#pragma once

// Capture orchestration driven by the AmigaDrivers wrapper (GoxDriverApp).
// Owns the session directory, the per-camera CameraSessions, and the
// periodic stats/limits/PTP loop. The injected StopController is driven
// from an external terminate flag via run_until_stop()'s predicate.

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
        // recording.output_dir/<session_name>; non-empty -> used verbatim,
        // name = basename. Never throws for control flow: on failure the
        // partially started sessions are torn down and exit_code() holds the
        // documented code.
        [[nodiscard]] bool init(const std::string &session_dir_override);

        // Blocks in the 200 ms poll loop (max_duration_s, stats, PTP refresh).
        // external_stop (optional) is polled every iteration; true requests
        // StopReason::External. Returns immediately if init() failed.
        void run_until_stop(const std::function<bool()> &external_stop = {});

        // stop_and_join all sessions. Idempotent. Returns the exit code
        // (0 clean, 1 with drops, 3/4/5/6 startup, 130 interrupted).
        int shutdown();

        int exit_code() const { return exit_code_; }
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
        bool initialized_ = false;
        bool shutdown_done_ = false;
        int exit_code_ = 0;
    };
} // namespace jai
