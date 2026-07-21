# Amiga Drivers

Unified data acquisition for four sensors — Septentrio **AsteRx** (GNSS/INS),
JAI **Go-X** GigE cameras, Aceinna **INS401** (INS/GNSS) and SICK **LMS4xxx**
2D LiDARs — running concurrently in one process with a shared logging,
configuration and lifecycle framework.

## Architecture

```
                      +--------------------------------------+
                      |         AmigaDrivers (main.cpp)      |
                      |  SignalHandler · spdlog (single      |
                      |  instance) · session folder · config |
                      |  snapshot · terminate propagation    |
                      +--+----------+-----------+---------+--+
                         |          |           |         |
                 AsterxDriverApp GoxDriverApp Ins401App Lms4xxxApp (xN instances)
                         |          |           |         |
                  Qt thread +   eBUS SDK    AF_PACKET   TCP CoLa-B client
                  SsnRx (TCP)   (GVSP)      raw socket  + SPSC ring buffer
                         |          |        + NTRIP    + writer thread
                     SBF files  jai-raw-seg  6 bin/rtcm  scan_*.bin
                                 segments    /nmea files
```

Every driver app implements the same duck-typed interface consumed by
`main.cpp`: `bool init([external_stop])` / `void run()` / `void shutdown()` /
`std::atomic<bool>& TerminateFlag()`. Each app runs `run()` on its own thread;
the main thread polls all terminate flags and propagates the first termination
to everyone (orderly join + shutdown).

### Targets and libraries

| Target | Description |
|--------|-------------|
| `AmigaDrivers` | The unified executable (the only acquisition entry point) |
| `jai_discover` / `jai_snapshot` | GigE enumeration / one-shot frame grab (used by the web GUI) |
| `jai_fake_capture` | SDK-free synthetic-frame test of the gox storage chain |
| `asterx_lib`, `gox_lib`, `ins401_lib`, `lms4xxx_lib` | Per-driver static libraries |
| `amiga_common` | Shared infrastructure: logging, config loading, signal handling, SPSC ring buffer, GUI marker contract |

### Dependencies

| Dependency | Provided by | Used by |
|-----------|-------------|---------|
| spdlog v1.17.0 (+fmt) | FetchContent, pinned in `3rd_party/FetchContent/` | all (single process-wide logger) |
| yaml-cpp 0.9.0 | FetchContent, pinned in `3rd_party/FetchContent/` | main + asterx/ins401/lms4xxx configs |
| nlohmann/json 3.11.3 | vendored `3rd_party/nlohmann/` | gox (strict JSONC config, session metadata) |
| doctest 2.4.11 | vendored `3rd_party/doctest/` | common + gox unit tests |
| Boost (header-only) | system | lms4xxx (Asio TCP), ins401 (CRC) |
| OpenSSL | system | ins401 (NTRIP Base64) |
| Eigen3 | system | ins401 (orientation math) |
| Qt5 Core/Network/SerialPort | system | asterx (vendored Septentrio SsnRx SDK) |
| eBUS SDK for JAI 6.6.1 | installed in the devcontainer | gox (GigE Vision) |
| GoogleTest v1.14 | FetchContent (asterx tests only) | asterx unit tests |
| GeographicLib v2.7 | FetchContent (ins401 tests only, fetched on Debug configure) | ins401 CompareGravityAccuracy test |

## Building

All C++ builds run inside the `amiga-sensor-dev` Docker container (the repo is
mounted at `/workspace`):

```bash
docker exec -w /workspace amiga-sensor-dev bash -lc \
  'cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)'
docker exec -w /workspace amiga-sensor-dev bash -lc \
  'ctest --test-dir build --output-on-failure'
```

Binaries land in `build/bin/`. Debug builds enable the unit tests
(`common_tests`, `jai_tests` — doctest; `asterx_test_*` — GoogleTest) by
default; Release builds skip them.

## Running

```bash
./build/bin/AmigaDrivers [path/to/config-main.yaml]   # default: config/config-main.yaml
```

`config/config-main.yaml` selects the drivers (`Enable ASTERX/GOX/INS401/LMS4XXX`),
their per-driver config paths and the `Output Directory`. Each run creates a
session folder:

```
<Output Directory>/<YYYYMMDD_HHMMSS>/
├── log_<ts>.log          # unified trace-level log (the GUI's primary feed)
├── config/               # snapshot of every enabled driver's config
└── bin/<driver>/         # asterx: *.sbf · gox: jai-raw-seg segments
                          # ins401: gnss/ins/imu/diagnostic .bin + .rtcm3/.nmea
                          # lms4xxx: scan_<instance>_<ts>.bin
```

Shutdown: `Ctrl+C` or `SIGTERM` — the signal handler sets the shared terminate
flag and every driver flushes and closes in order ("All drivers shut down" in
the log marks a clean exit).

Privileges: ins401 needs `CAP_NET_RAW` (raw Ethernet socket); lms4xxx requests
`SCHED_FIFO`/CPU affinity and degrades with a warning without `CAP_SYS_NICE`.
The web GUI applies `setcap` automatically before starting.

## Web GUI

`app/` is a NiceGUI control panel that runs on the host (`uv run amiga-gui`,
Python env in `.venv/`) and drives the container binaries. It starts/stops
`AmigaDrivers`, tails the session log, tracks per-sensor health, edits configs
and previews Go-X snapshots.

**Contract**: the GUI parses the log by line format
(`[HH:MM:SS] [level] [Module]: msg`) and by verbatim lifecycle marker strings.
Both are frozen in a single source of truth per side —
`common/include/driver_markers.h` (C++) mirrored by `app/services/markers.py`
(Python). After editing either, run:

```bash
uv run python tools/check_contracts.py
```

`common_tests` additionally asserts the C++ log output matches the GUI's line
regex.

## Repository layout

```
amiga_drivers/
├── main.cpp                  # unified entry point
├── CMakeLists.txt            # top-level build
├── cmake/FetchContent.cmake  # spdlog + yaml-cpp (sources pinned in-repo)
├── 3rd_party/                # pinned FetchContent sources + vendored single-header libs
├── common/                   # amiga_common + tests/
├── asterx_driver/            # Qt/SsnRx session, SBF writer, GoogleTest tests
├── gox_driver/               # jai_core (SDK-free) + jai_ebus (eBUS glue) + tools/ + tests/
├── ins401_driver/            # receiver, NTRIP client, static initialization
├── lms4xxx_driver/           # TCP CoLa-B driver, scan parser, record writer
├── parse/                    # DataConverter: offline bin -> CSV (currently not in the build)
├── app/                      # NiceGUI web GUI (host-side, uv-managed .venv)
└── tools/                    # repo tooling (contract checker, ...)
```

Per-driver details live in `asterx_driver/README.md` and `gox_driver/README.md`
(ins401/lms4xxx have no per-driver README yet).
