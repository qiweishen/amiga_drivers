#pragma once

// PvResult error plumbing for the SDK-dependent layer: a readable rendering
// of PvResult, an exception type carrying the failed result, and the
// CHECK_PV macro used for "this must succeed or the whole operation fails"
// SDK calls.

#include <PvResult.h>
#include <PvString.h>
#include <stdexcept>
#include <string>

namespace jai::ebus {

	// PvString -> std::string (PvString::GetAscii can return NULL).
	inline std::string to_std(const PvString &s) {
		const char *p = s.GetAscii();
		return p == nullptr ? std::string() : std::string(p);
	}

	// "OK", or "TIMEOUT (Operation timed out)" style rendering built from
	// GetCodeString() and GetDescription().
	std::string pv_result_to_string(const PvResult &result);

	class SdkError : public std::runtime_error {
	public:
		SdkError(const std::string &what, const PvResult &result);
		explicit SdkError(const std::string &what);

		const PvResult &result() const { return result_; }

	private:
		PvResult result_;
	};

}  // namespace jai::ebus

// Evaluates a PvResult-returning expression and throws SdkError (carrying
// the result) when it is not OK. `what` is a short description of the call.
#define CHECK_PV(expr, what)                                       \
	do {                                                           \
		const PvResult check_pv_result_ = (expr);                  \
		if (!check_pv_result_.IsOK()) {                            \
			throw ::jai::ebus::SdkError((what), check_pv_result_); \
		}                                                          \
	} while (0)
