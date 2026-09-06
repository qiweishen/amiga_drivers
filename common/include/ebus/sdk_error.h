#pragma once

#include <PvResult.h>
#include <PvString.h>
#include <stdexcept>
#include <string>

namespace common::Ebus {
    // PvString -> std::string (PvString::GetAscii can return NULL).
    inline std::string ToStd(const PvString &s) {
        const char *p = s.GetAscii();
        return p == nullptr ? std::string() : std::string(p);
    }

    // "OK", or "TIMEOUT (Operation timed out)" style rendering built from
    // GetCodeString() and GetDescription().
    std::string PvResultToString(const PvResult &result);

    class SdkError : public std::runtime_error {
    public:
        SdkError(const std::string &what, const PvResult &result);

        explicit SdkError(const std::string &what);

        const PvResult &Result() const { return result_; }

    private:
        PvResult result_;
    };
} // namespace common::Ebus

// Evaluates a PvResult-returning expression and throws SdkError (carrying
// the result) when it is not OK. `what` is a short description of the call.
#define CHECK_PV(expr, what)                                          \
	do {                                                              \
		const PvResult check_pv_result_ = (expr);                     \
		if (!check_pv_result_.IsOK()) {                               \
			throw ::common::Ebus::SdkError((what), check_pv_result_); \
		}                                                             \
	} while (0)
