#include "ebus/sdk_error.h"

namespace common::Ebus {
    std::string PvResultToString(const PvResult &result) {
        std::string code = ToStd(result.GetCodeString());
        std::string desc = ToStd(result.GetDescription());
        if (code.empty()) {
            code = "code " + std::to_string(result.GetCode());
        }
        if (desc.empty()) {
            return code;
        }
        return code + " (" + desc + ")";
    }

    SdkError::SdkError(const std::string &what, const PvResult &result)
        : std::runtime_error(what + ": " + PvResultToString(result)), result_(result) {
    }

    SdkError::SdkError(const std::string &what) : std::runtime_error(what), result_(PvResult::Code::GENERIC_ERROR) {
    }
} // namespace common::Ebus
