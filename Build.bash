#!/usr/bin/env bash
set -e

# Single Pleora eBUS SDK 6.5.1 serves both the JAI Go-X and the Specim FX10
# (mirrors .devcontainer/Dockerfile).
if [ ! -d /opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64 ]; then
    sudo dpkg -i resource/eBUS_SDK_Ubuntu-22.04-x86_64-6.5.1-6797.deb || sudo apt-get install -f -y
fi

rm -rf build
mkdir build && cd build
cmake ..
make -j"$(nproc)"

sudo setcap cap_sys_nice+ep ./bin/AmigaDrivers
