#!/usr/bin/env bash
set -euo pipefail

# Host udev manages the dongle's permissions and stable SDK-compatible alias.
if [[ -e /.dockerenv ]]; then
    printf 'Run start.bash on the Docker host.\n' >&2
    exit 1
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd -- "$project_dir"
keylok_rules="$project_dir/resource/99-pleora-keylok.rules"
compose_file="$project_dir/.devcontainer/docker-compose.yml"

if [[ ! -r "$keylok_rules" ]]; then
    printf 'KEYLOK udev rule is missing or unreadable: %s\n' "$keylok_rules" >&2
    exit 1
fi

sudo install -D -m 0644 "$keylok_rules" /etc/udev/rules.d/99-pleora-keylok.rules
sudo udevadm control --reload-rules

# Reloading rules only affects later events; allow startup without a dongle.
if [[ ! -c /dev/usb/hid/hiddev0 ]]; then
    printf 'KEYLOK alias is not present. Insert or replug the dongle to apply the rule before opening eBUSPlayer.\n' >&2
fi

sudo sysctl -w net.core.rmem_max=33554432

# Keep the existing image and C++ build workflow.
docker compose -f "$compose_file" up -d --build --remove-orphans
docker exec -w /workspace amiga-drivers-dev bash -lc \
  'cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)'
