#include "ebus/env_bootstrap.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "logger.h"

// Compile-time GenICam env constants baked from the root cmake/FindeBUS.cmake
// results (target_compile_definitions on amiga_ebus, common/CMakeLists.txt).
#ifndef AMIGA_EBUS_GENICAM_ENV_NAME
#define AMIGA_EBUS_GENICAM_ENV_NAME "GENICAM_ROOT_V3_4"
#endif
#ifndef AMIGA_EBUS_GENICAM_ROOT
#define AMIGA_EBUS_GENICAM_ROOT ""
#endif


namespace {
    common::DriverLog g_log{"eBUS"};
}

namespace common::Ebus {
    void BootstrapEnv() {
        constexpr const char *name = AMIGA_EBUS_GENICAM_ENV_NAME;
        constexpr const char *root = AMIGA_EBUS_GENICAM_ROOT;
        if (name[0] == '\0') {
            return; // built without SDK detection; nothing to bootstrap
        }
        const char *current = std::getenv(name);
        if (current != nullptr && current[0] != '\0') {
            g_log.Trace("env bootstrap: {} already set to {}", name, current);
            return;
        }
        if (root[0] == '\0') {
            g_log.Warn("env bootstrap: {} is not set and no compile-time GenICam root is known; "
                       "source <sdk>/bin/set_puregev_env.sh if SDK initialization fails",
                       name);
            return;
        }
        if (::setenv(name, root, /*overwrite=*/0) != 0) {
            g_log.Warn("env bootstrap: setenv({}) failed: {}", name, std::strerror(errno));
            return;
        }
        g_log.Trace("env bootstrap: {}={} (compile-time detected)", name, root);
    }
} // namespace common::Ebus
