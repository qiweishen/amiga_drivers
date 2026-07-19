#include "ebus/env_bootstrap.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "core/logger.hpp"
#include "version.hpp"

namespace jai::ebus {

	void bootstrap_env() {
		const char *name = version::kGenicamEnvName;
		if (name == nullptr || name[0] == '\0') {
			return;	 // built without SDK detection; nothing to bootstrap
		}
		const char *current = std::getenv(name);
		if (current != nullptr && current[0] != '\0') {
			LOG_DEBUG("env bootstrap: ", name, " already set to ", current);
			return;
		}
		if (version::kGenicamRoot == nullptr || version::kGenicamRoot[0] == '\0') {
			LOG_WARN("env bootstrap: ", name, " is not set and no compile-time GenICam root is known; ",
					 "source <sdk>/bin/set_puregev_env.sh if SDK initialization fails");
			return;
		}
		if (::setenv(name, version::kGenicamRoot, /*overwrite=*/0) != 0) {
			LOG_WARN("env bootstrap: setenv(", name, ") failed: ", std::strerror(errno));
			return;
		}
		LOG_INFO("env bootstrap: ", name, "=", version::kGenicamRoot, " (compile-time detected)");
	}

}  // namespace jai::ebus
