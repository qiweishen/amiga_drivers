#pragma once

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
        static constexpr std::uint32_t kFineTime = 1u << 6; // ReceiverStatus.RxState bit 6

        int min_uptime_s{1200};
        bool require_finetime{true};

        std::optional<std::uint32_t> up_time_s; // last ReceiverStatus.UpTime
        bool finetime{false};

        // Returns true when the up-time went backwards (receiver reset)
        bool Observe(std::uint32_t up_time, std::uint32_t rx_status) {
            const bool went_back = up_time_s.has_value() && up_time < *up_time_s;
            up_time_s = up_time;
            finetime = (rx_status & kFineTime) != 0;
            return went_back;
        }

        bool Ready() const {
            if (!up_time_s) {
                return min_uptime_s <= 0 && !require_finetime;
            }
            return static_cast<std::int64_t>(*up_time_s) >= min_uptime_s && (!require_finetime || finetime);
        }

        // "up 07:12 / 20:00, FINETIME not yet set" — only the pending conditions are named
        std::string Progress() const {
            if (!up_time_s) {
                return "no ReceiverStatus received yet";
            }
            std::string out = "up " + FormatHms(*up_time_s);
            if (min_uptime_s > 0) {
                out += " / " + FormatHms(static_cast<std::uint32_t>(min_uptime_s));
            }
            if (require_finetime && !finetime) {
                out += ", FINETIME not yet set";
            }
            return out;
        }

        void Reset() {
            up_time_s.reset();
            finetime = false;
        }
    };
} // namespace asterx
