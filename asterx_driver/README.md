# asterx_driver

A slim C++ driver that records raw IMU + dual-antenna GNSS observations from a
**Septentrio AsteRx-i3 D Pro+** receiver over Ethernet and writes raw SBF
bytes to rotating `.sbf` files. Receiver communication, SBF framing/CRC and
all post-processing are delegated to **Septentrio RxTools v26.1.0** — the
driver itself is only the glue (configuration sequence, geometry-parameter
verification, rotating writer, reconnect policy).

Target platform: Ubuntu 22.04, C++17, CMake, Qt5. **No ROS.**

## AmigaDrivers 统一入口集成

本 driver 已并入仓库根部的 **AmigaDrivers** 统一工程（与 ins401 / lms4xxx / gox 同级）：

- **构建**：由根 `CMakeLists.txt` 的 `add_subdirectory(asterx_driver)` 引入，产出静态库
  `asterx_lib`（含统一入口适配层 `AsterxDriverApp`，见 `include/asterx_driver_app.h`）；
  spdlog/yaml-cpp/Threads 由 `amiga_common` 提供，Qt5 由 devcontainer 提供。
- **运行**：`config/config-main.yaml` 中设 `Enable ASTERX: true`，
  `ASTERX Driver Config Path` 指向本目录 `config/config-asterx.yaml`。
  `output.dir` 被覆盖为 `<Output Directory>/<时间戳>/bin/asterx/`
  （与其他传感器同会话目录），日志走统一 log 文件（`log.level` 仍作过滤，
  带 `[AsteRx]: ` 前缀）。启动失败（接收机不可达/命令被拒/几何参数回读不一致）
  或写盘失败会关停整个进程；首配成功后的断线只重连（新 `.sbf` 分段），
  不影响其他驱动。standalone 入口（asterx_driver / asterx_probe）已移除。

## Architecture

```
Record (one thread, ONE TCP connection to <host>:28784):
  receiver ──TCP──> SSN::SsnRx (vendored RxTools SDK, 3rd_party/RxTools)
                      ├─ command replies ──> Session state machine
                      │     login → configure → verify IMU orientation /
                      │     INS lever arm / GNSS attitude (+offset) readbacks
                      ├─ SBF blocks (CRC-validated) ──> rotating .sbf files
                      └─ link loss / silence ──> reconnect + full reconfigure

Post-process (offline, scripts/postprocess.sh, prebuilt RxTools/bin tools):
  X.sbf ─ bin2asc -m ExtSensorMeas1 → X_imu.csv  IMU, ms timestamps (PRIMARY)
        ─ sbf2asc -j            → X_imu.asc    IMU quick-look (see caveats)
        ─ sbf2asc -a -s -u      → X_att.asc    AttEuler/AttCovEuler/AuxPos
        ─ sbf2rin -a 1 -nO -s -D → X_ant1.obs  main-antenna RINEX v3.04 obs
        ─ sbf2rin -a 2 -nO -s -D → X_ant2.obs  aux-antenna RINEX v3.04 obs
        ─ sbf2rin -nP           → X.nav        mixed navigation (ephemerides)
```

SBF output is targeted at the driver's own connection descriptor (e.g.
`IP10`), so the streams die with the connection — no stale streams, one
reconnect path. The RINEX pair (`ant1`/`ant2` + `nav`) loads directly in
RTKLIB for dual-antenna processing.

## Prerequisites

1. **Building needs no RxTools installation**: the Septentrio communication
   SDK (`ssnrx`) is vendored, byte-identical and unmodified, in
   `3rd_party/RxTools/` (see the README there) — so the recorder builds
   self-contained, e.g. on an edge device.
   **Post-processing** does need a local **RxTools v26.1.0** installation (or
   symlink) at `./RxTools` for the prebuilt converters (`bin2asc`, `sbf2asc`,
   `sbf2rin`); point `RXTOOLS_BIN` elsewhere if it lives in another path.
2. System packages (already in `.devcontainer/Dockerfile`):

```bash
sudo apt install -y \
    build-essential cmake ninja-build pkg-config \
    qtbase5-dev libqt5serialport5-dev
```

（spdlog / yaml-cpp 由父工程 FetchContent 经 `amiga_common` 提供，无需系统包。）

## Build

Built from the repo root as part of AmigaDrivers:

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
# Unit tests (GoogleTest; default ON for Debug builds, or -DASTERX_BUILD_TESTS=ON):
cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc) && ctest --output-on-failure
```

The driver is linked into `build/bin/AmigaDrivers` as `asterx_lib`.

## Run

`config/config-main.yaml` 设 `Enable ASTERX: true` 后运行 `./build/bin/AmigaDrivers`
（先编辑 `asterx_driver/config/config-asterx.yaml`：至少 connection.host、
connection.password 与 IMU→天线杆臂 `receiver.imu.ant_lever_arm_m`）。

- Output: `<Output Directory>/<timestamp>/bin/asterx/asterx-YYYYMMDDTHHMMSSZ-N.sbf`,
  rotated at 1 GiB or 1 h (must stay < 2 GiB — the RxTools converters refuse
  larger files).
- The geometry parameters (IMU orientation, INS lever arm, GNSS attitude and
  offset) are **read back and verified** after being set; a mismatch aborts
  startup. Set `log.level: debug` to record every command and verbatim
  receiver reply in the unified log file as a configuration audit trail.
- Link loss, command errors after startup, or 30 s of SBF silence trigger:
  close segment → reconnect → full reconfigure. Every reconnect starts a new
  `.sbf` segment. First-launch failures stop the whole process instead of
  retrying.
- Ctrl-C stops cleanly (closing the connection also stops the streams).

## Post-processing

```bash
./scripts/postprocess.sh <data>/<timestamp>/bin/asterx/asterx-20260718T090000Z-1.sbf [outdir]
./scripts/postprocess.sh <data>/<timestamp>/bin/asterx/   # convert every segment
```

Environment: `RXTOOLS_BIN` overrides the tools directory;
`KEEP_INVALID_TIME=0` drops rows with invalid timestamps (IMU samples recorded
in `Boot` startup mode before GNSS time was known — default keeps them).

### IMU output format

**Use `*_imu.csv` (bin2asc) for the estimator.** It is comma-separated with a
title row (`-t`), keeps the raw `TOW` [ms] / `WNc` fields at millisecond
resolution, and decodes each ExtSensorMeas sub-block type into its proper
fields. `Type` values (SBF Reference Guide / `RxTools/sbf2asc/sbfdef.h`
`EXTSENSORSBF_MEASTYPE_*`): 0 = acceleration [m/s²], 1 = angular rate
[deg/s], 2 = magnetic field, 3 = sensor info (temperature), 4 = velocity,
20 = zero-velocity flag.

`*_imu.asc` (`sbf2asc -j`, rows tagged `-13`) is kept for quick eyeballing
only — the vendor tool has three documented-by-inspection limitations
(`RxTools/sbf2asc/sbf2asc.c:835`):

1. the time column is printed with **0.01 s resolution**, so 200 Hz IMU rows
   get duplicate/aliased timestamps — unusable for INS integration;
2. every `-13` row carries **6 trailing literal `0` columns** (13 tokens per
   row, not 7);
3. X/Y/Z are only meaningful for **Type 0 and Type 1** rows; Type 3/4 rows are
   printed through the wrong union member and are garbage.

`*_att.asc` rows: `-4` AttEuler (heading/pitch/roll, deg), `-5` AttCovEuler,
`-12` AuxPos (aux-antenna ΔEast/ΔNorth/ΔUp, m). On-demand extras:
`RxTools/bin/sbfblocks -f X.sbf` (block inventory),
`RxTools/bin/bin2asc -f X.sbf -m <Block> -x -t` (any block as self-describing
text; block names via `bin2asc -l`).

## Layout

```
include/                — project headers (session.hpp, commands.hpp,
                          sbf_writer.hpp, app_config.hpp, asterx_log.hpp,
                          asterx_driver_app.h — unified-entry wrapper)
config/config-asterx.yaml — driver config (referenced by config-main.yaml)
src/
├── asterx_driver_app.cpp — AsterxDriverApp: hosts the Qt event loop on a worker
│                         thread behind init/run/shutdown for the unified main
├── session.cpp         — connection state machine on SSN::SsnRx (configure,
│                         verify readbacks, reconnect, watchdog)
├── commands.cpp        — pure functions: command list + reply verifiers
├── sbf_writer.cpp      — rotating .sbf writer (CRC-validated blocks in)
└── app_config.cpp      — YAML config loading/validation
3rd_party/RxTools/ssnrx — vendored Septentrio SDK (unmodified; see its README)
scripts/postprocess.sh  — bin2asc/sbf2asc/sbf2rin wrapper (see above)
```

`CMakeLists.txt` builds everything as a subdirectory of the top-level
AmigaDrivers project (no per-directory CMake files except `tests/`).

## Third-party notices

- This driver vendors the Septentrio RxTools "GNSS Receiver Communication
  SDK for C++/Qt" (`ssnrx`) sources, byte-identical and unmodified, in
  `3rd_party/RxTools/`, and invokes the prebuilt `bin2asc` / `sbf2asc` /
  `sbf2rin` converters from a locally installed RxTools at post-processing
  time. The RxTools license ("Exclusions") permits modifying, compiling and
  using the provided sbf2asc and C++/Qt SDK sources, including as part of
  another software product, at the user's own risk (see
  `RxTools/Licenses/RxTools License.txt`).
- **RxTools itself (binaries, bundled Qt, manuals) must NOT be redistributed**
  without Septentrio's written consent — it is deliberately untracked and
  gitignored; install it locally from the Septentrio installer.
- RxTools bundles Qt 6.8.2 under LGPLv3 (`RxTools/Licenses/LGPL3.txt`), used
  only to run Septentrio's own tools as shipped. This driver links the Ubuntu
  system Qt 5.15 dynamically, which satisfies LGPLv3 for our binary.

## License

BSD-3-Clause (see `LICENSE`) for the driver's own code.
