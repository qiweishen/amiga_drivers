#pragma once

#include <cstdlib>

// GenICam environment bootstrap for processes started without sourcing the
// SDK's set_puregev_env.sh: without GENICAM_ROOT_V3_4 the very first Pv* call
// fails with a cryptic error (same rationale as gox's jai::ebus::bootstrap_env,
// kept fx10-local to avoid a cross-driver dependency). The values are baked at
// configure time from the root cmake/FindeBUS.cmake results; setenv with
// overwrite=0 keeps an already-provisioned environment (devcontainer ENV,
// profile.d) authoritative. Call before the first eBUS SDK call.

#ifndef FX10_GENICAM_ENV_NAME
#define FX10_GENICAM_ENV_NAME "GENICAM_ROOT_V3_4"
#endif
#ifndef FX10_GENICAM_ROOT
#define FX10_GENICAM_ROOT ""
#endif

namespace fx10 {
    inline void bootstrapGenicamEnv() {
        if (FX10_GENICAM_ROOT[0] != '\0') {
            ::setenv(FX10_GENICAM_ENV_NAME, FX10_GENICAM_ROOT, /*overwrite=*/0);
        }
    }
} // namespace fx10
