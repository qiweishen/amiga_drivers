#pragma once

// GenICam runtime environment bootstrap. The eBUS SDK's GenICam layer needs
// GENICAM_ROOT_V3_4 (name detected at configure time, see version.hpp) to
// point at <sdk>/lib/genicam. When the process is started without sourcing
// set_puregev_env.sh, the very first Pv* call would fail with a cryptic
// "GENICAM_ROOT ... is not set" error; bootstrap_env() fixes that up front.

namespace jai::ebus {

	// If getenv(version::kGenicamEnvName) is unset and version::kGenicamRoot is
	// non-empty, setenv it (never overrides an existing value). Must be the very
	// first thing main() does, before any eBUS SDK call. Safe to call more than
	// once. Logs a single line describing what happened.
	void bootstrap_env();

}  // namespace jai::ebus
