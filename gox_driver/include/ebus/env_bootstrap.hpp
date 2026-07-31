#pragma once

// GenICam runtime environment bootstrap. The eBUS SDK's GenICam layer needs
// GENICAM_ROOT_V3_4 (name and root baked at configure time as the
// GOX_GENICAM_* compile definitions) to point at <sdk>/lib/genicam. When the
// process is started without sourcing set_puregev_env.sh, the very first Pv*
// call would fail with a cryptic "GENICAM_ROOT ... is not set" error;
// bootstrap_env() fixes that up front.

namespace jai::ebus {
    // If the GenICam env variable is unset and a compile-time root is known,
    // setenv it (never overrides an existing value). Must be the very first
    // thing main() does, before any eBUS SDK call. Safe to call more than
    // once. Logs a single line describing what happened.
    void bootstrap_env();
} // namespace jai::ebus
