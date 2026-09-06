# Amiga Drivers

**Unified data acquisition for a four-sensor mobile mapping rig.** One process,
one log, one session folder — a GNSS/INS receiver, two GigE Vision cameras and a
set of 2D LiDARs recording concurrently behind a shared logging, configuration
and lifecycle framework, with a web control panel on top.

The acquisition time reference is **AsteRx GPS time**. Field captures use
SensorSync driven by AsteRx PPS and ZDA; its ZDA-to-GPS conversion must be
established from the receiver configuration. Other device clocks and host
clocks are diagnostic only and must never be used as an association fallback.
Tests without SensorSync may record images without cross-sensor time association.
Host monotonic clocks are used only for operational deadlines and statistics.

All four apps share `Init → Run → Shutdown` and a sticky `HasFailed()` result.
Shutdown drains accepted data before releasing resources; a recording/close
failure takes precedence over an operator stop and produces a failed run with
exit code 1. Protocol-specific device commands retain their device semantics.
Session names remain second-resolution and existing GoX output may be overwritten.

<p align="left">
  <img alt="C++20"    src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <img alt="CMake"    src="https://img.shields.io/badge/CMake-%E2%89%A5%203.14-064F8C?logo=cmake&logoColor=white">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Ubuntu%2022.04%20(devcontainer)-E95420?logo=ubuntu&logoColor=white">
  <img alt="GUI"      src="https://img.shields.io/badge/GUI-NiceGUI%20%C2%B7%20uv-3776AB?logo=python&logoColor=white">
  <img alt="Storage"  src="https://img.shields.io/badge/storage-SBF%20%C2%B7%20ENVI%20%C2%B7%20HDF5-4B8BBE">
</p>

> A former fifth driver (Aceinna INS401) is retired and archived outside this
> repository.

## Contents

- [Sensors](#sensors) · [Architecture](#architecture) · [Quick start](#quick-start)
- [Configuration](#configuration) · [Session output](#session-output) · [Health guards](#health-guards)
- [Logging conventions](#logging-conventions) · [Web GUI](#web-gui)
- [Development](#development) · [Repository layout](#repository-layout) · [Documentation](#documentation)

## Sensors

| Sensor | Driver | Transport | Records |
|---|---|---|---|
| Septentrio **AsteRx-i3 D Pro+** (GNSS/INS) | `asterx_driver` | Qt + vendored SsnRx SDK over TCP | `*.sbf` blocks + `live_*.csv` telemetry sidecar |
| JAI **Go-X** GigE cameras (×N) | `gox_driver` | Pleora eBUS SDK (GVSP) | `jai-raw-seg` segments + `device.json` + `telemetry.jsonl` per camera |
| Specim **FX10e** hyperspectral pushbroom | `fx10_driver` | Pleora eBUS SDK (GVSP) | ENVI BIL `.bil`/`.hdr` + SensorSync `sensor_trigger.log` |
| SICK **LMS4000** 2D LiDARs (×N) | `lms4xxx_driver` | CoLa B binary over TCP 2111 | `scan_<id>_<ts>_NNN.h5` (`lms4xxx-h5` v3) |

Every driver **resets the device to a known baseline on each start** and then
writes only the parameters the recording depends on, reading each one back.
No driver ever writes network/IP settings — the single IP writer in the code base
is the operator-triggered `ebus_set_ip` tool behind *Camera Tools → Set IP*.
Per-device rules, telegram-by-telegram, live in each driver's
[`docs/DEVICE_CONFIG.md`](#documentation).

## Architecture

```
                      +---------------------------------------------------+
                      |              AmigaDrivers (main.cpp)              |
                      |  SignalHandler · spdlog (single instance) ·       |
                      |  session folder · config snapshot · drivers.json  |
                      |  · terminate propagation · exit status            |
                      +--+----------+----------+----------+---------------+
                         |          |          |          |
                 AsterxDriverApp GoxApp    Fx10App    Lms4xxxApp (xN)
                         |          |          |          |
                  Qt thread +   eBUS SDK   eBUS SDK   TCP CoLa-B client
                  SsnRx (TCP)   (GVSP)     (GVSP)     + SPSC ring buffer
                         |          |          |      + writer thread
                     SBF files  jai-raw-seg ENVI BIL  scan_*.h5
                                 segments   + trig log
```

Every driver app implements the same duck-typed interface consumed by
`main.cpp`: `bool init([external_stop])` / `void run()` / `void shutdown()` /
`std::atomic<bool>& TerminateFlag()`. Each app runs `run()` on its own thread;
the main thread polls all terminate flags and propagates the first termination
to everyone (orderly join + shutdown).

**A failed run is visible from the outside.** `main` exits `0` on a clean or
signal-interrupted run and `1` when a driver ended the run itself, and
`drivers.json` in the session folder records `completed`, `interrupted (signal N)`
or `failed (<driver>)`. Without that, a rig that died thirty seconds into a
two-hour session was indistinguishable from one that finished.

### Targets and libraries

| Target | Description |
|--------|-------------|
| `AmigaDrivers` | The unified executable — the only acquisition entry point |
| `ebus_discover` / `ebus_set_ip` | GigE Vision enumeration for both camera drivers / camera re-addressing (FORCEIP + persistent IP); both live in `common/` and are used by the web GUI |
| `jai_snapshot` / `fx10_snapshot` | One-shot Go-X frame grab / FX10 waterfall preview grab (web GUI). `fx10_snapshot --dump-features` also prints the camera's whole GenICam feature list, which is how `features.map` node names are found on real hardware |
| `asterx_lib`, `fx10_lib`, `gox_lib`, `lms4xxx_lib` | Per-driver static libraries |
| `amiga_common` | Shared infrastructure: logging, config loading, signal handling, SPSC ring buffer, GUI marker contract |
| `amiga_ebus` | The eBUS-SDK-dependent layer shared by gox and fx10 (`common/include/ebus`): GenICam env bootstrap, PvResult errors, discovery, device IP configuration |

### Dependencies

| Dependency | Provided by | Used by |
|-----------|-------------|---------|
| HDF5 1.14.6 (C library, static `hdf5-static`) | FetchContent, pinned in `3rd_party/FetchContent/` (`cmake/Dependencies.cmake`) | lms4xxx scan recorder (`lms4xxx-h5`, see `docs/FORMAT_H5.md`) |
| zlib (`zlib1g-dev`) | system (devcontainer apt) | HDF5 gzip filter — a hard requirement of the pinned HDF5 CMake |
| spdlog v1.17.0 (+fmt) | FetchContent, pinned in `3rd_party/FetchContent/` | all (single process-wide logger) |
| yaml-cpp 0.9.0 | FetchContent, pinned in `3rd_party/FetchContent/` | main + asterx/fx10/lms4xxx configs |
| nlohmann/json v3.12.0 | FetchContent, pinned in `3rd_party/FetchContent/` | gox (`jai-raw-seg` `idx.jsonl`, device sidecars, snapshot tools) + `drivers.json` |
| doctest 2.4.11 | vendored `3rd_party/doctest/` | common + gox + lms4xxx unit tests |
| GoogleTest v1.14 | FetchContent (asterx); apt fallback (fx10) | asterx + fx10 unit tests |
| Boost (header-only) | system | lms4xxx (Asio TCP) |
| Qt5 Core/Network/SerialPort | system | asterx (vendored Septentrio SsnRx SDK) |
| eBUS SDK (Pleora) 6.5.1 | installed in the devcontainer (single SDK, root `cmake/FindeBUS.cmake`) | gox + fx10 (GigE Vision) |

## Quick start

### Prerequisites

```bash
git clone --recurse-submodules <repo>          # submodule/sensor_trigger is REQUIRED:
cd amiga_drivers                               # fx10_core fails to configure without it
git submodule update --init                    # ...or this, on an existing clone
```

The C++ side builds inside the `amiga-drivers-dev` devcontainer (repo mounted at
`/workspace`), which brings the eBUS SDK, Qt5, Boost and zlib. `Build.bash`
installs the SDK `.deb` from `resource/` if it is missing.

### Build

```bash
docker exec -w /workspace amiga-drivers-dev bash -lc \
  'cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)'
```

Binaries land in `build/bin/`. Unit tests are **on in Debug, off in Release**, so
a Release `ctest` runs nothing — see [Development](#development).

### Run

```bash
sudo setcap cap_sys_nice+ep ./build/bin/AmigaDrivers   # lms4xxx SCHED_FIFO; Start.bash does this
./build/bin/AmigaDrivers [path/to/config-main.yaml]    # default: config/config-main.yaml
```

Stop with `Ctrl+C` or `SIGTERM`: the signal handler sets the shared terminate
flag and every driver flushes and closes in order. `All drivers shut down` in the
log marks a clean exit.

> Without `CAP_SYS_NICE` the lms4xxx receive thread degrades from `SCHED_FIFO`
> to normal scheduling with a warning rather than failing. The web GUI applies
> the capability automatically before starting.

### Web control panel

```bash
uv run amiga-gui        # http://localhost:8619 (host-side, Python env in .venv/)
```

See [Web GUI](#web-gui) for what each page does.

## Configuration

`config/config-main.yaml` is the only file the executable is given. It selects
the drivers, their per-driver config paths and the output root:

```yaml
General:
    Output Directory: ./data
    Enable ASTERX: false
    Enable FX10: true
    Enable GOX: true
    Enable LMS4XXX: false
    ASTERX Driver Config Path: ./asterx_driver/config/config-asterx.yaml
    FX10 Driver Config Path: ./fx10_driver/config/config-fx10.yaml
    GOX Driver Config Path: ./gox_driver/config/config-gox.yaml
    LMS4XXX Driver Config Path: ./lms4xxx_driver/config/config-lms4xxx.yaml
Guards:                        # rig-wide, see Health guards below
    Disk Min Free GiB: 5       # stop the whole run below this (0 = off)
    Disk Warn Free GiB: 20
    No Data Warn S: 5
    No Data Abort S: 60        # any sensor silent this long stops the run (0 = off)
Logging System:
    Enable Logging: true
```

The `Guards:` values are range-checked at load, and a warning threshold that
could never fire before its stop threshold is rejected rather than accepted and
ignored — a guard that is silently misconfigured is worse than no guard, because
the log still says it is armed.

Each driver config is a heavily commented YAML template next to its driver. The
templates are **part of the documentation** — every non-obvious value carries the
manual page it comes from — so the GUI edits them as text rather than
re-serialising them.

**One schema policy for all four drivers** (`common/include/config_util.h`):
unknown keys are startup errors that name the key and list the accepted set,
omitted keys keep the driver defaults, every error carries the dotted key path,
and every numeric key is range-checked (a negative value into an unsigned slot is
refused by name, never wrapped). The templates share one layout — banner,
`device:`/`cameras:`/`lidar:`, acquisition, driver extras, `output:`, `network:`,
`logging:` — one comment style (short trailing notes: unit, range, allowed
values) and one unit-suffix rule (`_s`, `_ms`, `_hz`, `_mb`, `_bytes`, `_deg`).
Each driver's tests load its shipped template, so template rot fails the build.

## Session output

Each run creates one session folder:

```
<Output Directory>/<YYYYMMDD_HHMMSS>/
├── log_<ts>.log                  # unified trace-level log (the GUI's primary feed)
├── drivers.json                  # run header, versions, per-driver enable/config, final status
├── config/                       # snapshot of every enabled driver's config
└── raw/
    ├── asterx/                   # *.sbf blocks + live_*.csv telemetry sidecar
    ├── gox/<cam>/                # jai-raw-seg segments + idx.jsonl
    │                             #   + device.json (one-shot audit) + telemetry.jsonl
    ├── fx10/<base>_<UTC>Z/       # ENVI segment_NNNN.bil + .hdr, sensor_trigger.log
    └── lms4xxx/                  # scan_<instance>_<ts>_NNN.h5
```

Two recording formats are self-describing enough to read without this repository:

- **ENVI BIL** (fx10) — a segment is valid **iff its `.hdr` exists**; the header is
  written last, and it carries the read-back exposure and frame rate, the AIE
  state and which calibration pack applies.
- **`lms4xxx-h5`** (lms4xxx) — `/frames/*` per-scan metadata, `/channels/*` raw
  `[scan, point]` values with their scale factors, and `/telemetry/*` device
  health. HDF5 has no atomic rename, so a finished file is marked instead: **a
  file without the `closed_cleanly` root attribute is incomplete.** Flushes are
  `H5Fflush` + `fdatasync`, so the loss window really is `flush_interval_ms`.
  `lms4xxx_driver/scripts/inspect_h5.py` is the reference reader
  (`info` / `verify` / `csv`).

## Health guards

Recording silently producing nothing is the failure mode that matters. Two of
those rules are rig-wide and live in `main.cpp`, driven by the `Guards:` block
of `config/config-main.yaml`:

| Guard | What it watches | Default |
|---|---|---|
| Disk floor | Free space (GiB, `f_bavail`) on the filesystem holding the session folder — checked once before bring-up and once a second afterwards | warn below 20, stop below 5 |
| No-data watchdog | `IDriverApp::MicrosSinceLastData()` per driver: how long that sensor has produced nothing | warn after 5 s, stop after 60 s |

They are rig-wide because every driver writes under
`<Output Directory>/<timestamp>/raw/` (one filesystem, one number), and because
one dead sensor already makes the session incomplete. The abort default is 60 s
rather than something snappier because AsteRx repairs its own 30 s SBF silence by
reconnecting: a rig-wide abort at 30 s would race that and end a run the driver
was about to fix. A driver that is
*legitimately* quiet — an external trigger with no pulses, a receiver warm-up, a
camera between reconnect attempts — reports `nullopt` and is simply not watched
while that lasts; deciding *that* is the one part the drivers keep, because only
they know it. A watchdog trip is logged as `<Driver> driver stopped: no data
(...)`, which is what turns that sensor's GUI card red.

On top of the two, each driver keeps the guards that are specific to its device:

| Driver | Guards |
|---|---|
| asterx | Warm-up gate on `ReceiverStatus` up-time / FINETIME before any block is recorded; a 30 s SBF silence timer that **reconnects** (recovery is the driver's job; giving up is main's) |
| gox | PTP offset/step guard, thermal warning, `Counter0` missed-trigger accounting, queue-depth shedding |
| fx10 | *Stream-unusable* abort — buffers keep arriving but none is usable, which total silence cannot detect and main therefore cannot see — thermal limits (processing board 80 °C / FPGA 90 °C), recorder-failure classification |
| lms4xxx | First-telegram content verification, NTP server probe, consecutive framing-error threshold, writer failure |

Any of these ends the whole rig's run — and says so in the exit status
(`drivers.json` records `failed (<driver>)`, or `failed (disk)`).

The two snapshot tools (`fx10_snapshot`, `jai_snapshot`) do not run under
`main`, so `Guards:` never applies to them; each keeps its own 10 s no-frame
bound so a GUI preview of a dead camera fails fast instead of waiting out its
capture budget.

## Logging conventions

Every line is `[HH:MM:SS] [level] [Module]: msg`. On top of that, the drivers
follow four rules.

**1. Module tags are two-layered.** Only `*_driver_app.cpp` logs under the App
token (`AsteRxApp` / `FX10App` / `GoXApp` / `LMS4xxxApp` from
`driver_markers.h`) — those are the lines the GUI health state machine reacts to
(an App-level `error` marks the sensor FAILED). Every other file in a driver logs
under the short internal module (`AsteRx` / `FX10` / `GoX` / `LMS4xxx`), which
the GUI displays but never routes: a transient internal warning must not flip a
sensor's health. Shared `common/` components use neutral modules
(`DriversJson`) — never a driver's name.

**2. Message prefixes identify the instance, then the subsystem.**
Multi-instance drivers tag every line with the instance first: `[cam0] ...` (gox
cameras), `[Front_Center_Laser] ...` (lms instances). On App-level `error` lines
this leading tag is load-bearing: the GUI routes the failure to that instance (no
tag = all instances). Subsystem files add a fixed second-level prefix after the
instance tag: `[Writer]` (segment/file writers), `[eBUS]` (Pleora SDK
control/stream code), `[TCP]` (socket transport). Special-purpose sidecars keep
their own tag in the same style (`[Live]` asterx CSV feed, `[TriggerLog]` fx10
SensorSync session). Example:
`[LMS4xxx]: [Front_Center_Laser] [Writer] Recording to ...`.

**3. Statistics are uniform across drivers.** Periodic status lines start with
`[Statistics]` (plus the instance tag where applicable), use double-space-separated
`key={}` fields, and report two measured rates over the same window — `rate=N.N Hz`,
the sensor's own output rate, and `fps=N.N`, the frames that actually reached
disk. **A gap between the two *is* the loss.** New fields are appended, never
interleaved. The shutdown totals line is `[Statistics] [inst] Final: ...`.

```
[FX10App]:    [Statistics] frames=1200  rate=50.0 Hz  fps=49.8  missed_triggers=0  temp_pcb=41.2  temp_fpga=52.7
[GoX]:        [Statistics] [cam0] up=00:01:05  rate=24.1 Hz  fps=24.0  disk=119.8 MB/s  ok=1560  incomp=0  drop_q=0  ...
[LMS4xxxApp]: [Statistics] [Front_Center_Laser] up=00:00:10  rate=600.0 Hz  fps=598.0  ntp=OK  frames=6000  ...  queued=600.0  temp=41.2  unexpected=0
[AsteRx]:     [Statistics] blocks=48210  bytes=12.4 MB  files=1  crc_fail=0  len_fail=0  discarded=0  ...
```

`fps=` is a GUI contract: `app/services/driver_stats.py` parses it into the
dashboard's per-sensor cards, and `tools/check_contracts.py` fails if either side
renames it. AsteRx records a byte stream rather than frames, so it has no `fps=`;
during the receiver warm-up its line carries `warmup=<up>/<min>  finetime=0|1`
instead.

**4. Throwing is an app-layer decision.** `common::DriverLog` never throws by
default; the explicit `g_log.Error(true, ...)` overload (log, then
`std::runtime_error` with the formatted message) is the only sanctioned throw in
driver code and is reserved for `*_driver_app.cpp`. Lower layers propagate
failures upward instead — `std::error_code` returns (lms4xxx) or driver-internal
exception types (`RecorderError`, `TransportError`, `SdkError`, …) that the app
layer catches — and the app layer decides whether to abort.
(`common::Log::LogAndThrow` remains, but only `main.cpp` and `common/` use it.)

Lifecycle markers (`GoX driver initialized`, `LiDAR instance [x] initialized
successfully`, …) are verbatim GUI contract strings. For lms4xxx the per-instance
markers come from each instance app and the driver-level pair is aggregated by
`main.cpp` once all instances are up / down.

## Web GUI

`app/` is a NiceGUI control panel that runs on the **host** (`uv run amiga-gui`,
port 8619, Python env in `.venv/`) and drives the container binaries.

| Page | What it does |
| --- | --- |
| `/` **Overview** | Start/stop a recording; per-driver Enable switches (written to `config-main.yaml`, applied at the next start); one card per sensor — health state, bytes on disk, live written-frames rate — plus the output-disk gauge. |
| `/config` **Config** | Raw-text editor for every driver config: comments survive, the YAML is validated, and the save is atomic and leaves a `.bak`. |
| `/logs` **Logs** | The unified session log with level/module filters and follow mode. |
| `/live` **Data Live** | On-demand preview of the RUNNING session: each click reads the last second straight out of the files the drivers are writing (newest Go-X frame, FX10 spectrum). |
| `/asterx` **AsteRx Live** | Live telemetry from the `live_*.csv` sidecars — INS map track, a dynamic FRD attitude triad (heading/pitch/roll), and INS / receiver health panels. |
| `/camera` **Camera Tools** | One shared device discovery for both camera drivers, then a Go-X and an FX10 exposure workbench. |

**Camera Tools** covers what the Go-X and FX10 pages used to do separately. One
Scan runs `ebus_discover --json` into a single table — every GigE Vision camera on
the host's adapters, classified by vendor (JAI → Go-X, Specim → FX10) with its
subnet mask, IP-configuration state and subnet validity; picking a row targets
that driver's workbench. Three rules shape the page:

- *The GigE control channel is exclusive.* Discovery is a broadcast the cameras
  answer without it, so the scan always runs; a camera owned by the running
  recording is marked "in use" and its snapshot / Set IP buttons stay disabled.
  Conversely `process.preflight` refuses to start a recording while a camera tool
  (snapshot or Set IP) is in flight.
- *Set IP is the only thing that ever writes a camera's network settings.* The
  eBUS SDK exposes just a transient FORCEIP, so `ebus_set_ip` does two steps:
  FORCEIP by MAC so the camera becomes reachable, then a connect that writes
  `GevPersistentIPAddress/SubnetMask/DefaultGateway` and enables
  `GevCurrentIPConfigurationPersistentIP` so the address survives power cycles.
  The dialog refuses an address the host NIC cannot reach (override under
  Advanced), reports a persist failure as "transient only", and — when the
  checkbox is ticked and the write persisted — rewrites `device.ip` in the
  driver's config so the next recording connects to the new address.
- *Preview never writes the config.* Exposure, gain and the FX10
  spatial/spectral binning are passed to the snapshot tools as CLI overrides, so
  trying a setting cannot mutate what the next recording uses. **Apply to config**
  is the explicit writeback: it edits the driver config as text so the
  documentation in it survives, re-reads the value back before saving, and takes
  effect at the next recording start. For Go-X it writes the `cameras:` entry that
  identifies the selected camera — by MAC when the entry is MAC-bound (the driver
  ignores `device.ip` there), otherwise by IP.

**Latency.** A sensor state change surfaces in the browser after three stages: the
driver's log flush (`err` and above immediately, the rest within 200 ms —
`common/src/logger.cpp`), the tailer's `LOG_POLL_S`, and the page's
`UI_TICK_S`/`TOOL_TICK_S` (`app/constants.py`). spdlog flushes nothing by default,
so leaving that first stage out would strand the GUI a whole stdio buffer (~9 s at
this project's log rate) behind reality.

## Development

### Tests

Unit tests are enabled in Debug builds and skipped in Release:

```bash
docker exec -w /workspace amiga-drivers-dev bash -lc \
  'cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build-debug -j$(nproc)'
docker exec -w /workspace amiga-drivers-dev bash -lc \
  'ctest --test-dir build-debug --output-on-failure'
```

| Suite | Framework | ctest granularity |
|---|---|---|
| `common_tests` | doctest | one entry |
| `gox_tests`, `fx10_tests`, `asterx_tests`, `lms4xxx_tests` | doctest | one entry **per test file** (`--source-file=` filter) |

All of them are **device-free**: protocol frames, parsers, config schemas,
recorders and the on-disk formats are exercised against synthetic input, most of
it built from the vendor manuals' own field tables rather than from the
implementation.

### Contracts

Three things are duplicated between C++ and Python by necessity, and a checker
keeps them honest:

```bash
uv run python tools/check_contracts.py
```

It verifies the lifecycle markers (`common/include/driver_markers.h` ↔
`app/services/markers.py`), the `[Statistics]` `fps=` field of every frame-based
driver, and that the GUI's parser really reads a rendered line of each.
`common_tests` additionally asserts the C++ log output matches the GUI's line
regex, and that an error line reaches the file without an explicit flush.

**Run it after touching any marker string or any `[Statistics]` format string.**

## Repository layout

```
amiga_drivers/
├── main.cpp                  # unified entry point: session folder, threads, exit status
├── CMakeLists.txt            # top-level build
├── Build.bash / Start.bash   # convenience wrappers (SDK install, setcap, run)
├── cmake/                    # Dependencies.cmake (hdf5/spdlog/yaml-cpp/nlohmann) + FindeBUS.cmake
├── 3rd_party/                # pinned FetchContent sources + vendored single-header libs
├── config/config-main.yaml   # driver selection + output root
├── common/                   # amiga_common + amiga_ebus + tests/
├── asterx_driver/            # Qt/SsnRx session, SBF writer, live CSV sidecar, tests/
├── fx10_driver/              # fx10_core (SDK-free) + fx10_ebus (eBUS glue) + tools/ + tests/
├── gox_driver/               # jai_core (SDK-free) + jai_ebus (eBUS glue) + tools/ + tests/
├── lms4xxx_driver/           # CoLa B driver, scan parser, HDF5 recorder, scripts/, tests/
├── submodule/sensor_trigger/ # Teensy SensorSync-Logger host client (fx10 hardware trigger)
├── app/                      # NiceGUI web GUI (host-side, uv-managed .venv)
└── tools/                    # repo tooling (contract checker, …)
```

## Documentation

| Document | Covers |
|---|---|
| [`asterx_driver/docs/DEVICE_CONFIG.md`](asterx_driver/docs/DEVICE_CONFIG.md) | Reset to `RxDefault` on every connect, account handling, the warm-up gate |
| [`gox_driver/docs/DEVICE_CONFIG.md`](gox_driver/docs/DEVICE_CONFIG.md) | `UserSetLoad Default` on every bring-up, the ordered apply plan, the raw-feature audit |
| [`fx10_driver/docs/DEVICE_CONFIG.md`](fx10_driver/docs/DEVICE_CONFIG.md) | Factory user set + calibration-ROI guard, the write-order plan, which parameters are exposed and why the rest stay at their factory values |
| [`lms4xxx_driver/docs/DEVICE_CONFIG.md`](lms4xxx_driver/docs/DEVICE_CONFIG.md) | `mSCloadappdef` baseline, every telegram with its page number, `sAN` status-byte polarity, the shutdown handshake, the device self-report |
| [`lms4xxx_driver/docs/FORMAT_H5.md`](lms4xxx_driver/docs/FORMAT_H5.md) | The `lms4xxx-h5` layout, durability and completeness semantics |
| [`lms4xxx_driver/docs/sopas_filter_polarity.md`](lms4xxx_driver/docs/sopas_filter_polarity.md) | Why two filter status bytes contradict the manual's tables |
| [`TODO.md`](TODO.md) | Roadmap and the log of completed milestones |

The device-config documents are the ones to read before changing anything that
talks to hardware: they record not just what the driver writes, but what it
deliberately does **not** write, and why — with the manual page for each decision.
