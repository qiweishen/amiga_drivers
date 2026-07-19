# env_ebus.sh -- locate the eBUS SDK and load its runtime environment into the CURRENT shell.
#
# Usage (must be sourced, not executed):
#   source scripts/env_ebus.sh
#
# Search order: $EBUS_SDK_ROOT, then /opt/jai/ebus_sdk/*, then /opt/pleora/ebus_sdk/* -- the first
# directory containing bin/set_puregev_env.sh wins. That SDK script exports GENICAM_ROOT_V3_x,
# PUREGEV_ROOT and extends LD_LIBRARY_PATH. Exports EBUS_SDK_ROOT for FindeBUS.cmake and friends.
#
# The driver binary itself normally does NOT need this: its RPATH points at the SDK lib directories
# and EnvBootstrap sets GENICAM_ROOT_V3_4 at startup. Source this for SDK tools (eBUS Player,
# SoftDeviceGEV samples) or when running binaries built against a relocated SDK.
#
# No `set -e` and no `exit` here: this file runs in the caller's interactive shell.

_jai_env_ebus_sdk=""

if [ -n "${EBUS_SDK_ROOT:-}" ] && [ -f "${EBUS_SDK_ROOT}/bin/set_puregev_env.sh" ]; then
    _jai_env_ebus_sdk=${EBUS_SDK_ROOT}
else
    for _jai_env_ebus_cand in /opt/jai/ebus_sdk/* /opt/pleora/ebus_sdk/*; do
        if [ -f "${_jai_env_ebus_cand}/bin/set_puregev_env.sh" ]; then
            _jai_env_ebus_sdk=${_jai_env_ebus_cand}
            break
        fi
    done
    unset _jai_env_ebus_cand
fi

if [ -z "${_jai_env_ebus_sdk}" ]; then
    echo "env_ebus.sh: no eBUS SDK found (checked \$EBUS_SDK_ROOT, /opt/jai/ebus_sdk/*," >&2
    echo "             /opt/pleora/ebus_sdk/*). Install the eBUS SDK for JAI .deb first." >&2
    unset _jai_env_ebus_sdk
    # `return` works when sourced; the guarded `exit` only triggers if someone executed this file.
    return 1 2>/dev/null || exit 1
fi

export EBUS_SDK_ROOT=${_jai_env_ebus_sdk}
echo "env_ebus.sh: sourcing ${EBUS_SDK_ROOT}/bin/set_puregev_env.sh"

# Some SDK releases resolve paths relative to their own directory; source it from bin/ to be safe.
_jai_env_ebus_oldpwd=$(pwd)
cd "${EBUS_SDK_ROOT}/bin" || return 1 2>/dev/null || exit 1
. ./set_puregev_env.sh
cd "${_jai_env_ebus_oldpwd}" || true

unset _jai_env_ebus_sdk _jai_env_ebus_oldpwd
