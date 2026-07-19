#!/usr/bin/env bash
set -e

if [ ! -d /opt/jai/ebus_sdk/Ubuntu-22.04-x86_64 ]; then
    sudo dpkg -i gox_driver/resource/eBUS_SDK_JAI_Ubuntu-22.04-x86_64-6.6.1-7475.deb || sudo apt-get install -f -y
fi

rm -rf build
mkdir build && cd build
cmake ..
make -j"$(nproc)"

sudo setcap cap_net_raw,cap_sys_nice+ep ./bin/AmigaDrivers
