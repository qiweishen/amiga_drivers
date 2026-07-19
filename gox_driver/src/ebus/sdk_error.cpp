#include "ebus/sdk_error.hpp"

namespace jai::ebus {

	std::string pv_result_to_string(const PvResult &result) {
		std::string code = to_std(result.GetCodeString());
		std::string desc = to_std(result.GetDescription());
		if (code.empty()) {
			code = "code " + std::to_string(result.GetCode());
		}
		if (desc.empty()) {
			return code;
		}
		return code + " (" + desc + ")";
	}

	SdkError::SdkError(const std::string &what, const PvResult &result) :
		std::runtime_error(what + ": " + pv_result_to_string(result)), result_(result) {}

	SdkError::SdkError(const std::string &what) : std::runtime_error(what), result_(PvResult::Code::GENERIC_ERROR) {}

}  // namespace jai::ebus
