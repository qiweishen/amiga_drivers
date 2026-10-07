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
} // namespace gox
