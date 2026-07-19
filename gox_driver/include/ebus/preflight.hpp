#pragma once

// Host/container network self-checks for user-mode GVSP reception. Under
// host networking the sysctls we read are the host's, but /proc/sys may be
// read-only inside the container — so the checks read, verify, attempt the
// harmless fixes, and otherwise print the exact host-side command. The
// structured report goes to the log and into session.json;
// preflight.fail_on_error decides (in the caller) whether Error entries
// block startup.

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/config.hpp"

namespace jai::ebus {

	enum class CheckLevel { Ok, Info, Warn, Error };

	const char *check_level_name(CheckLevel level);

	struct CheckResult {
		std::string name;
		CheckLevel level = CheckLevel::Ok;
		std::string message;
	};

	struct PreflightReport {
		std::vector<CheckResult> checks;

		bool has_error() const;
		nlohmann::ordered_json to_json() const;
		void log() const;  // one LOG_* line per check, level-mapped
	};

	// Checks: rp_filter (all + relevant interfaces; !=0 && !=2 is fatal for GVSP
	// unicast, tries to write 0 first), net.core.rmem_max vs the configured
	// socket rx buffer, MTU vs explicit packet_size+28, configured local_ip
	// present on a local interface (host-netns sanity), firewall advisory (UDP
	// 3956 + GVSP data port), and the eBUS kernel module (informational; this
	// driver always uses the user-mode receiver).
	PreflightReport run_preflight(const AppConfig &cfg);

}  // namespace jai::ebus
