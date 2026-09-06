#include "ptp_status.h"

#include <string>

#include "string_util.h"

namespace gox {
    PtpState ClassifyPtpStatus(const std::string &status) {
        const std::string s = common::StringUtil::ToLower(common::StringUtil::Trim(status));
        if (s == "slave" || s == "9") {
            return PtpState::kSlave;
        }
        if (s == "master" || s == "6") {
            return PtpState::kMaster;
        }
        if (s == "faulty" || s == "2") {
            return PtpState::kFaulty;
        }
        return PtpState::kOther;
    }

    bool PtpClockAccuracyOk(int64_t accuracy) {
        return accuracy >= 0 && accuracy <= 9;
    }

    bool PtpClockAccuracyFromName(const std::string &text, int64_t &out) {
        // Manual p.128, in register order. The names carry the accuracy, so a
        // firmware that only exposes the entry name is still guardable.
        static const char *kEntries[] = {
            "within25ns", "within100ns", "within250ns", "within1us", "within2p5us",
            "within10us", "within25us", "within100us", "within250us", "within1ms",
            "within2p5ms", "within10ms", "within25ms", "within100ms", "within250ms",
            "within1s", "within10s", "greaterthan10s", "alternateptpprofile", "unknown",
            "reserved",
        };
        const std::string s = common::StringUtil::ToLower(common::StringUtil::Trim(text));
        if (s.empty()) {
            return false;
        }
        for (size_t i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i) {
            if (s == kEntries[i]) {
                out = static_cast<int64_t>(i);
                return true;
            }
        }
        // Some firmware prints the value instead of the name.
        if (s.size() > 18 || s.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        out = std::stoll(s);
        return true;
    }
} // namespace gox
