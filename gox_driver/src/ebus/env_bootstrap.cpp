#include "ebus/env_bootstrap.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "logger.h"

// Compile-time GenICam env constants baked from the root cmake/FindeBUS.cmake
// results (same mechanism as fx10's ebus_env.hpp).
#ifndef GOX_GENICAM_ENV_NAME
#define GOX_GENICAM_ENV_NAME "GENICAM_ROOT_V3_4"
#endif
#ifndef GOX_GENICAM_ROOT
#define GOX_GENICAM_ROOT ""
#endif


namespace {
    Common::DriverLog g_log{"GoX"};
}

namespace jai::ebus {
    void bootstrap_env() {
        constexpr const char *name = GOX_GENICAM_ENV_NAME;
        constexpr const char *root = GOX_GENICAM_ROOT;
        if (name[0] == '\0') {
            return; // built without SDK detection; nothing to bootstrap
        }
        const char *current = std::getenv(name);
        if (current != nullptr && current[0] != '\0') {
            g_log.trace("[eBUS] env bootstrap: {} already set to {}", name, current);
            return;
        }
        if (root[0] == '\0') {
            g_log.warn("[eBUS] env bootstrap: {} is not set and no compile-time GenICam root is known; "
                       "source <sdk>/bin/set_puregev_env.sh if SDK initialization fails",
                       name);
            return;
        }
        if (::setenv(name, root, /*overwrite=*/0) != 0) {
            g_log.warn("[eBUS] env bootstrap: setenv({}) failed: {}", name, std::strerror(errno));
            return;
        }
        g_log.trace("[eBUS] env bootstrap: {}={} (compile-time detected)", name, root);
    }
} // namespace jai::ebus
