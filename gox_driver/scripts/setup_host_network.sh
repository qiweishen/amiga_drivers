#!/usr/bin/env bash
#
# setup_host_network.sh -- prepare a network interface + kernel for GigE Vision capture (JAI Go-X).
#
# Usage:
#   sudo scripts/setup_host_network.sh <iface> [mtu]        # mtu defaults to 9000 (jumbo frames)
#
# What it configures (idempotent: every change is echoed, already-correct values are left untouched):
#   * MTU on <iface> (camera and switch must support jumbo frames too; packet size negotiation then
#     yields ~8164-byte GVSP packets instead of 1476)
#   * net.ipv4.conf.all.rp_filter=0 and net.ipv4.conf.<iface>.rp_filter=0 -- the reverse-path filter
#     silently drops GVSP streams the kernel considers "unroutable"
#   * net.core.rmem_max / net.core.rmem_default >= 32 MiB (only ever raised, never lowered);
#     PvStreamGEV::SetUserModeSocketRxBufferSize() is capped by rmem_max
#   * if an eBUS SDK installation is found (EBUS_SDK_ROOT, /opt/jai/ebus_sdk/*, /opt/pleora/ebus_sdk/*),
#     its own helpers bin/set_rp_filter.sh and bin/set_socket_buffer_size.sh are invoked first; the
#     direct sysctl fallback below then verifies/repairs the end state either way.
#
# Docker note: the devcontainer runs with network_mode:host, so these sysctls and the interface MTU ARE
# the host's. Run this script on the host, or inside the container (requires NET_ADMIN). In a container
# without enough privileges /proc/sys may be mounted read-only -- writes will fail and this script will
# tell you to re-run it on the host instead.

set -u

usage() {
    echo "usage: sudo $0 <iface> [mtu]" >&2
    echo "example: sudo $0 eth2 9000" >&2
    exit 2
}

[ $# -ge 1 ] && [ $# -le 2 ] || usage
IFACE=$1
MTU=${2:-9000}

case "$MTU" in
    ''|*[!0-9]*) echo "error: mtu must be a number, got '$MTU'" >&2; usage ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "error: must be run as root: sudo $0 $*" >&2
    exit 2
fi

if [ ! -d "/sys/class/net/$IFACE" ]; then
    echo "error: interface '$IFACE' not found. Available interfaces:" >&2
    ls /sys/class/net >&2
    exit 2
fi

RMEM_BYTES=$((32 * 1024 * 1024)) # 32 MiB
FAILURES=0

note_proc_sys_readonly() {
    echo "       -> /proc/sys appears not writable. If you are inside a container, re-run this" >&2
    echo "          script on the HOST (the container shares the host network namespace anyway)." >&2
}

# set_sysctl <key> <value>: write only when different, echo what happened.
set_sysctl() {
    key=$1
    want=$2
    cur=$(sysctl -n "$key" 2>/dev/null || echo "?")
    if [ "$cur" = "$want" ]; then
        echo "ok:    $key = $want (unchanged)"
        return 0
    fi
    if sysctl -w "$key=$want" >/dev/null 2>&1; then
        echo "set:   $key: $cur -> $want"
    else
        echo "ERROR: could not write $key (current: $cur, wanted: $want)" >&2
        note_proc_sys_readonly
        FAILURES=$((FAILURES + 1))
    fi
}

# raise_sysctl <key> <min>: like set_sysctl but never lowers an already-larger value.
raise_sysctl() {
    key=$1
    minval=$2
    cur=$(sysctl -n "$key" 2>/dev/null || echo 0)
    if [ "$cur" -ge "$minval" ] 2>/dev/null; then
        echo "ok:    $key = $cur (already >= $minval)"
    else
        set_sysctl "$key" "$minval"
    fi
}

echo "== setup_host_network: iface=$IFACE mtu=$MTU rmem>=$RMEM_BYTES =="

# ---------------------------------------------------------------------------------------------------
# 1. MTU
# ---------------------------------------------------------------------------------------------------
CUR_MTU=$(cat "/sys/class/net/$IFACE/mtu")
if [ "$CUR_MTU" = "$MTU" ]; then
    echo "ok:    $IFACE mtu = $MTU (unchanged)"
else
    if ip link set dev "$IFACE" mtu "$MTU"; then
        echo "set:   $IFACE mtu: $CUR_MTU -> $MTU"
    else
        echo "ERROR: could not set MTU $MTU on $IFACE (NIC/driver may not support jumbo frames)" >&2
        FAILURES=$((FAILURES + 1))
    fi
fi

# ---------------------------------------------------------------------------------------------------
# 2. eBUS SDK helper scripts, when an SDK is installed (preferred, then verified below anyway)
# ---------------------------------------------------------------------------------------------------
SDK_ROOT=""
if [ -n "${EBUS_SDK_ROOT:-}" ] && [ -d "$EBUS_SDK_ROOT/bin" ]; then
    SDK_ROOT=$EBUS_SDK_ROOT
else
    for d in /opt/jai/ebus_sdk/* /opt/pleora/ebus_sdk/*; do
        if [ -d "$d/bin" ]; then
            SDK_ROOT=$d
            break
        fi
    done
fi

if [ -n "$SDK_ROOT" ]; then
    echo "info:  using eBUS SDK helpers from $SDK_ROOT/bin"
    for helper in set_rp_filter.sh set_socket_buffer_size.sh; do
        script="$SDK_ROOT/bin/$helper"
        if [ -f "$script" ]; then
            echo "run:   $script"
            # Helpers differ slightly between SDK releases; treat them as best-effort and let the
            # direct sysctl pass below enforce the final state.
            if ! bash "$script" </dev/null; then
                echo "warn:  $helper returned non-zero; falling back to direct sysctl" >&2
            fi
        fi
    done
else
    echo "info:  no eBUS SDK found (EBUS_SDK_ROOT, /opt/jai/ebus_sdk/*, /opt/pleora/ebus_sdk/*);"
    echo "       applying sysctls directly"
fi

# ---------------------------------------------------------------------------------------------------
# 3. Verify / enforce the end state directly (covers SDK helper differences and no-SDK hosts)
# ---------------------------------------------------------------------------------------------------
set_sysctl "net.ipv4.conf.all.rp_filter" 0
set_sysctl "net.ipv4.conf.$IFACE.rp_filter" 0
raise_sysctl "net.core.rmem_max" "$RMEM_BYTES"
raise_sysctl "net.core.rmem_default" "$RMEM_BYTES"

echo
if [ "$FAILURES" -gt 0 ]; then
    echo "== done with $FAILURES failure(s) -- see errors above (run on the host?) ==" >&2
    exit 1
fi
echo "== done. Note: these settings do not survive a reboot; persist them via /etc/sysctl.d/ and"
echo "   your network manager's MTU setting if needed. =="
exit 0
