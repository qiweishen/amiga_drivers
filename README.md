# Amiga Drivers

Unified data acquisition for four sensors — Septentrio **AsteRx** (GNSS/INS),
JAI **Go-X** GigE cameras, Specim **FX10** hyperspectral line-scan camera and
SICK **LMS4xxx** 2D LiDARs — running concurrently in one process with a shared
logging, configuration and lifecycle framework. (A former fifth driver,
Aceinna INS401, is retired and archived outside this repository.)

## Architecture

```
                      +---------------------------------------------------+
                      |              AmigaDrivers (main.cpp)              |
                      |  SignalHandler · spdlog (single instance) ·       |
                      |  session folder · config snapshot · terminate     |
                      |  propagation                                      |
                      +--+----------+----------+----------+---------------+
                         |          |          |          |
                 AsterxDriverApp GoxApp    Fx10App    Lms4xxxApp (xN)
                         |          |          |          |
                  Qt thread +   eBUS SDK   eBUS SDK   TCP CoLa-B client
                  SsnRx (TCP)   (GVSP)     (GVSP)     + SPSC ring buffer
                         |          |          |      + writer thread
                     SBF files  jai-raw-seg ENVI BIL  scan_*.bin
                                 segments   + trig log
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
| `fx10_probe` / `fx10_snapshot` | FX10 link/discovery smoke test / GUI waterfall preview grab |
| `jai_fake_capture` | SDK-free synthetic-frame test of the gox storage chain |
| `asterx_lib`, `fx10_lib`, `gox_lib`, `lms4xxx_lib` | Per-driver static libraries |
| `amiga_common` | Shared infrastructure: logging, config loading, signal handling, SPSC ring buffer, GUI marker contract |

### Dependencies

| Dependency | Provided by | Used by |
|-----------|-------------|---------|
| spdlog v1.17.0 (+fmt) | FetchContent, pinned in `3rd_party/FetchContent/` | all (single process-wide logger) |
| yaml-cpp 0.9.0 | FetchContent, pinned in `3rd_party/FetchContent/` | main + asterx/lms4xxx configs |
| nlohmann/json v3.12.0 | FetchContent, pinned in `3rd_party/FetchContent/` | gox (jai-raw-seg idx.jsonl, snapshot tools) + drivers.json |
| doctest 2.4.11 | vendored `3rd_party/doctest/` | common + gox unit tests |
| Boost (header-only) | system | lms4xxx (Asio TCP) |
| Qt5 Core/Network/SerialPort | system | asterx (vendored Septentrio SsnRx SDK) |
| eBUS SDK (Pleora) 6.5.1 | installed in the devcontainer (single SDK, root `cmake/FindeBUS.cmake`) | gox + fx10 (GigE Vision) |
| GoogleTest v1.14 | FetchContent (asterx tests; apt fallback for fx10) | asterx + fx10 unit tests |

## Building

All C++ builds run inside the `amiga-sensor-dev` Docker container (the repo is
mounted at `/workspace`):

```bash
docker exec -w /workspace amiga-sensor-dev bash -lc \
  'cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)'
docker exec -w /workspace amiga-sensor-dev bash -lc \
  'ctest --test-dir build --output-on-failure'
```

Binaries land in `build/bin/`. Debug builds enable the unit tests
(`common_tests`, `jai_tests` — doctest; `asterx_test_*`, `fx10_tests` —
GoogleTest) by default; Release builds skip them.

## Running

```bash
./build/bin/AmigaDrivers [path/to/config-main.yaml]   # default: config/config-main.yaml
```

`config/config-main.yaml` selects the drivers
(`Enable ASTERX/FX10/GOX/LMS4XXX`), their per-driver config paths and
the `Output Directory`. Each run creates a session folder:

```
<Output Directory>/<YYYYMMDD_HHMMSS>/
├── log_<ts>.log          # unified trace-level log (the GUI's primary feed)
├── config/               # snapshot of every enabled driver's config
└── bin/<driver>/         # asterx: *.sbf · gox: jai-raw-seg segments
                          # fx10: ENVI .bil/.hdr + sensor_trigger.log sessions
                          # lms4xxx: scan_<instance>_<ts>.bin
```

Shutdown: `Ctrl+C` or `SIGTERM` — the signal handler sets the shared terminate
flag and every driver flushes and closes in order ("All drivers shut down" in
the log marks a clean exit).

Privileges: lms4xxx requests `SCHED_FIFO`/CPU affinity and degrades with a
warning without `CAP_SYS_NICE`; the web GUI applies `setcap` automatically
before starting.

## Logging conventions

Every line is `[HH:MM:SS] [level] [Module]: msg`. On top of that, the drivers
(asterx / fx10 / gox / lms4xxx) follow three rules:

**1. Module tags are two-layered.** Only `*_driver_app.cpp` logs under the
App token (`AsteRxApp` / `FX10App` / `GoXApp` / `LMS4xxxApp` from
`driver_markers.h`) — those are the lines the GUI health state machine reacts
to (an App-level `error` marks the sensor FAILED). Every other file in a
driver logs under the short internal module (`AsteRx` / `FX10` / `GoX` /
`LMS4xxx`), which the GUI displays but never routes: a transient internal
warning must not flip a sensor's health. Shared `common/` components use
neutral modules (`DriversJson`) — never a driver's name.

**2. Message prefixes identify the instance, then the subsystem.** Multi-
instance drivers tag every line with the instance first: `[cam0] ...` (gox
cameras), `[front_left_laser] ...` (lms instances). On App-level `error`
lines this leading tag is load-bearing: the GUI routes the failure to that
instance (no tag = all instances). Subsystem files add a fixed second-level
prefix after the instance tag: `[Writer]` (segment/file writers), `[eBUS]`
(Pleora SDK control/stream code), `[TCP]` (socket transport). Special-purpose
sidecars keep their own tag in the same style (`[Live]` asterx CSV feed,
`[TriggerLog]` fx10 SensorSync session). Example:
`[LMS4xxx]: [front_left_laser] [Writer] Recording to ...`.

**3. Statistics are uniform across drivers.** Periodic status lines start
with `[Statistics]` (plus the instance tag where applicable), use
double-space-separated `key={}` fields, and report two measured rates over the
same window — `rate=N.N Hz`, the sensor's own output rate, and `fps=N.N`, the
frames that actually reached disk (a gap between them *is* the loss). The
shutdown totals line is `[Statistics] [inst] Final: ...`. Examples:

```
[FX10App]: [Statistics] frames=1520  rate=50.0 Hz  fps=50.0  missed_triggers=0  temp=42.1 °C  disk_free=812.4 GB
[GoX]: [Statistics] [cam0] up=00:01:05  rate=24.1 Hz  fps=24.0  disk=119.8 MB/s  ...
[LMS4xxxApp]: [Statistics] [front_left_laser] up=00:00:30  rate=25.0 Hz  fps=25.0  ntp=ok  frames=750  ...
```

`fps=` is a GUI contract: `app/services/driver_stats.py` parses it into the
dashboard's per-sensor cards, and `tools/check_contracts.py` fails if either
side renames it (AsteRx records a byte stream, so it has no `fps=`).

**4. Throwing is an app-layer decision.** `Common::DriverLog` never throws by
default; the explicit `g_log.error(true, ...)` overload (log, then
`std::runtime_error` with the formatted message) is the only sanctioned throw
in driver code and is reserved for `*_driver_app.cpp`. Lower layers propagate
failures upward instead — `std::error_code` returns (lms4xxx) or
driver-internal exception types (`RecorderError`, `TransportError`,
`SdkError`, ...) that the app layer catches — and the app layer decides
whether to abort. (`Common::Log::log_and_throw` remains, but only `main.cpp`
and `common/` use it.)

Lifecycle markers ("`GoX driver initialized`", "`LiDAR instance [x]
initialized successfully`", ...) are verbatim GUI contract strings — see the
next section. For lms4xxx the per-instance markers come from each instance
app and the driver-level pair is aggregated by `main.cpp` once all instances
are up / down.

## Web GUI

`app/` is a NiceGUI control panel that runs on the host (`uv run amiga-gui`,
Python env in `.venv/`) and drives the container binaries. It starts/stops
`AmigaDrivers`, tails the session log, tracks per-sensor health, edits configs
and previews Go-X and FX10 snapshots.

**Contract**: the GUI parses the log by line format
(`[HH:MM:SS] [level] [Module]: msg`), by verbatim lifecycle marker strings, and
by the `[Statistics]` line's `fps=` field (the per-sensor disk write rate shown
live on the dashboard cards — `app/services/driver_stats.py`). The markers are
frozen in a single source of truth per side —
`common/include/driver_markers.h` (C++) mirrored by `app/services/markers.py`
(Python). After editing either, or any `[Statistics]` format string, run:

```bash
uv run python tools/check_contracts.py
```

`common_tests` additionally asserts the C++ log output matches the GUI's line
regex, and that an error line reaches the file without an explicit flush.

**Latency**: a sensor state change surfaces in the browser after three stages —
the driver's log flush (`err` and above immediately, the rest within 200 ms;
`common/src/logger.cpp`), the tailer's `LOG_POLL_S`, and the page's
`UI_TICK_S`/`TOOL_TICK_S` (`app/constants.py`). spdlog flushes nothing by
default, so leaving that first stage out would strand the GUI a whole stdio
buffer (~9 s at this project's log rate) behind reality.

## Repository layout

```
amiga_drivers/
├── main.cpp                  # unified entry point
├── CMakeLists.txt            # top-level build
├── cmake/                    # FetchContent.cmake (spdlog/yaml-cpp/nlohmann) + FindeBUS.cmake
├── 3rd_party/                # pinned FetchContent sources + vendored single-header libs
├── common/                   # amiga_common + tests/
├── asterx_driver/            # Qt/SsnRx session, SBF writer, GoogleTest tests
├── fx10_driver/              # fx10_core (SDK-free) + fx10_ebus (eBUS glue) + tools/ + tests/
├── gox_driver/               # jai_core (SDK-free) + jai_ebus (eBUS glue) + tools/ + tests/
├── lms4xxx_driver/           # TCP CoLa-B driver, scan parser, record writer
├── parse/                    # DataConverter: offline bin -> CSV
├── app/                      # NiceGUI web GUI (host-side, uv-managed .venv)
└── tools/                    # repo tooling (contract checker, ...)
```

Per-driver details live in `asterx_driver/README.md`, `fx10_driver/README.md`
and `gox_driver/README.md` (lms4xxx has no per-driver README yet).
