#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace asterx {
    inline std::string FormatHms(std::uint32_t seconds) {
        char buf[32];
        const auto h = seconds / 3600;
        const auto m = (seconds % 3600) / 60;
        const auto s = seconds % 60;
        if (h > 0) {
            std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
        } else {
            std::snprintf(buf, sizeof(buf), "%02u:%02u", m, s);
        }
        return buf;
    }


    struct WarmupGate {
        using Clock = std::chrono::steady_clock;
        static constexpr std::uint32_t kFineTime = 1u << 6; // ReceiverStatus.RxState bit 6

        int min_recording_s{90};
        bool require_finetime{false};

        std::optional<std::uint32_t> up_time_s; // last ReceiverStatus.UpTime
        bool finetime{false};


        // Returns true when the up-time went backwards (receiver reset)
        bool Observe(std::uint32_t up_time, std::uint32_t rx_status) {
            const bool went_back = up_time_s.has_value() && up_time < *up_time_s;
            up_time_s = up_time;
            finetime = (rx_status & kFineTime) != 0;
            return went_back;
        }


        // Called only after the first nonempty SBF block is accepted by the
        // recording queue. Later blocks, phase changes and reconnects preserve it.
        void StartRecording(Clock::time_point now = Clock::now()) {
            if (!recording_started_) recording_started_ = now;
        }


        std::optional<std::uint32_t> RecordingElapsedSeconds(Clock::time_point now = Clock::now()) const {
            if (!recording_started_) return std::nullopt;
            if (now < *recording_started_) return 0;
            return static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now - *recording_started_).count());
        }


        bool Ready(Clock::time_point now = Clock::now()) const {
            const bool duration_ready = min_recording_s <= 0 ||
                (recording_started_ && now - *recording_started_ >= std::chrono::seconds(min_recording_s));
            return duration_ready && (!require_finetime || finetime);
        }


        // Recording duration is independent of ReceiverStatus.UpTime.
        std::string Progress(Clock::time_point now = Clock::now()) const {
            const auto elapsed = RecordingElapsedSeconds(now);
            if (!elapsed) return "waiting for first recorded SBF block";
            std::string out = "recorded " + FormatHms(*elapsed);
            if (min_recording_s > 0) {
                out += " / " + FormatHms(static_cast<std::uint32_t>(min_recording_s));
            }
            if (require_finetime && !finetime) {
                out += ", FINETIME not yet set";
            }
            return out;
        }


        // A reconnect needs fresh receiver health, but the shared SBF recording
        // and its elapsed timer continue; any link gap is reported separately.
        void ResetReceiverStatus() {
            up_time_s.reset();
            finetime = false;
        }


        void Reset() {
            recording_started_.reset();
            ResetReceiverStatus();
        }

    private:
        std::optional<Clock::time_point> recording_started_;
    };
} // namespace asterx
