#!/usr/bin/env bash
#
# run_driver.sh -- convenience launcher: load the eBUS SDK environment, then exec
# the unified AmigaDrivers binary (the standalone jai_goxdriver was removed).
#
# Usage:
#   scripts/run_driver.sh [path/to/config-main.yaml]
#
# AmigaDrivers is linked with an RPATH pointing at the SDK and GenICam lib
# directories and bootstraps GENICAM_ROOT_V3_4 itself at startup, so running
# build/bin/AmigaDrivers directly normally works without this wrapper. Use it as
# a fallback when the SDK was moved/reinstalled at a different path than at
# build time, or when RPATH resolution fails for any other reason.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# Best-effort: a missing SDK environment is not fatal here, RPATH usually covers it (see above).
# shellcheck source=/dev/null
if ! source "$SCRIPT_DIR/env_ebus.sh"; then
    echo "run_driver.sh: warning: eBUS environment not loaded; relying on the binary's RPATH" >&2
fi

BIN="$SCRIPT_DIR/../../build/bin/AmigaDrivers"
if [ ! -x "$BIN" ]; then
    echo "run_driver.sh: error: AmigaDrivers not found at <repo>/build/bin." >&2
    echo "               Build the top-level AmigaDrivers project first (inside the devcontainer)." >&2
    exit 1
fi

exec "$BIN" "$@"
