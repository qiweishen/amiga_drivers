#!/usr/bin/env bash
# Run an X11 program in the running phenomate-drivers-dev container on the display
# of the calling shell: an `ssh -Y` session to the headless edge computer
# (DISPLAY=localhost:10.0 or similar; windows open on the SSH client), or a
# local desktop session. Run on the Docker host, again after every SSH login.
#
#   .devcontainer/x11-exec.sh <program> [args...]
#   .devcontainer/x11-exec.sh bash -c '"$EBUS_SDK_ROOT"/bin/eBUSPlayer'
set -euo pipefail

container=phenomate-drivers-dev
xauth_in_container=/tmp/.x11.xauth  # XAUTHORITY in docker-compose.yml

if [[ -e /.dockerenv ]]; then
    printf 'Run x11-exec.sh on the Docker host.\n' >&2
    exit 1
fi
if [[ $# -eq 0 ]]; then
    printf 'usage: %s <program> [args...]\n' "$0" >&2
    exit 2
fi
if [[ -z "${DISPLAY:-}" ]]; then
    printf 'DISPLAY is not set: log in with "ssh -Y <host>" (the SSH client needs a running X server).\n' >&2
    exit 1
fi
if ! command -v xauth >/dev/null; then
    printf 'xauth is not installed on this host; sshd needs it for X11 forwarding as well.\n' >&2
    exit 1
fi

# The SSH cookie is stored under this host's name; family ffff (FamilyWild)
# makes it match whatever address the client inside the container resolves.
cookie="$(xauth nlist "$DISPLAY" | sed -e 's/^..../ffff/')"
if [[ -z "$cookie" ]]; then
    printf 'No X11 cookie for DISPLAY=%s in %s.\n' "$DISPLAY" "${XAUTHORITY:-$HOME/.Xauthority}" >&2
    exit 1
fi
tmp="$(mktemp)"
trap 'rm -f -- "$tmp"' EXIT
printf '%s\n' "$cookie" | xauth -q -f "$tmp" nmerge -

# Copied into the container's own /tmp, not bind-mounted: the display number and
# cookie change with every SSH login, while the container keeps running.
docker exec -i "$container" sh -c "umask 077 && cat > $xauth_in_container" <"$tmp"

args=(-e "DISPLAY=$DISPLAY" -e "XAUTHORITY=$xauth_in_container")
if [[ "$DISPLAY" != :* ]]; then
    # X server on another machine (SSH forwarding): no shared memory with it and
    # no access to its GPU, so Qt must not use MIT-SHM and OpenGL renders in software.
    args+=(-e QT_X11_NO_MITSHM=1 -e LIBGL_ALWAYS_SOFTWARE=1)
fi
if [[ -t 0 && -t 1 ]]; then
    args+=(-it)
fi
exec docker exec "${args[@]}" "$container" "$@"
