# Amiga Drivers

**Unified data acquisition for a four-sensor mobile mapping rig.** One process,
one log, one session folder — a GNSS/INS receiver, two GigE Vision cameras and a
set of 2D LiDARs recording concurrently behind a shared logging, configuration
and lifecycle framework, with a web control panel on top.

**Time.** The acquisition time reference is **AsteRx GPS time**, and each sensor's
absolute time comes from its own source only: the AsteRx stamps its SBF blocks in
GPS time and serves NTP and PTP on its own address (GPS timescale); the LMS4xxx
synchronise to it over NTP, the Go-X over PTP, and the FX10 is timed through
SensorSync (strobe edges against AsteRx PPS/ZDA — the ZDA-to-GPS conversion must
be established from the receiver configuration). Host clocks are **never** a time
source and are never added to the recorded data: the `host_*` columns some
formats carry are diagnostics, and host monotonic time is used only for
operational deadlines and statistics. A driver whose time source is switched off
in its yaml (`ntp.enabled`, `ptp.enabled`, `sensor_trigger.enabled`) warns at
start-up, because its data can then not be associated with anything offline.
Tests without SensorSync record FX10 images without cross-sensor time association.

**Fail-fast.** All four apps share `Init → Run → Shutdown` and a sticky
`HasFailed()` result. The first data-loss event in any driver — a damaged or
dropped SBF block, a link loss while recording, a dropped or incomplete camera
frame, a BlockID gap, a LiDAR ring/queue overflow, telegram counter gap or NTP
time-lock fault — stops the whole rig immediately: an incomplete recording is
never kept running, the operator restarts and re-captures. Shutdown drains
accepted data before releasing resources; a recording/close failure takes
precedence over an operator stop and produces a failed run with exit code 1.
Protocol-specific device commands retain their device semantics. Session folders
are named at second resolution and an existing one is refused, never reused.

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
| Septentrio **AsteRx RBi3 Pro+** (GNSS/INS; the rig's NTP + PTP server) | `asterx_driver` | Qt + vendored SsnRx SDK over TCP | `*.sbf` blocks (+ `prewarm/` context) + `live_*.csv` telemetry sidecar |
| JAI **Go-X** GigE cameras (×N) | `gox_driver` | Pleora eBUS SDK (GVSP), PTP slave | `jai-raw-seg` segments + `idx.jsonl` + `device.json` + `telemetry.jsonl` per camera |
| Specim **FX10e** hyperspectral pushbroom | `fx10_driver` | Pleora eBUS SDK (GVSP) | ENVI BIL `.bil`/`.hdr` + `.lines.csv` + SensorSync `sensor_trigger.log` |
| SICK **LMS4xxx** 2D LiDARs (LMS4121R / LMS4124R family, ×N) | `lms4xxx_driver` | CoLa B binary over TCP 2111, NTP client | `scan_<id>_<ts>_NNN.h5` (`lms4xxx-h5` v3) |

Every driver **resets the device to a known baseline on each start** and then
writes only the parameters the recording depends on, reading each one back.
No driver ever writes network/IP settings — the single IP writer in the code base
is the operator-triggered `ebus_set_ip` tool behind *Camera Tools → Set IP*.
Per-device rules, telegram-by-telegram, live in each driver's
[`docs/DEVICE_CONFIG.md`](#documentation); the rig's addresses and the time
server are listed in `resource/devices_ip.yaml`.

## Architecture

```
                      +---------------------------------------------------+
                      |              AmigaDrivers (main.cpp)              |
                      |  SignalHandler · spdlog (single instance) ·       |
                      |  session folder · config snapshot · drivers.json  |
                      |  · rig-wide guards · terminate propagation ·      |
                      |  exit status                                      |
                      +--+----------+----------+----------+---------------+
                         |          |          |          |
                 AsterxDriverApp  GoxApp    Fx10App    Lms4xxxApp (xN)
                         |          |          |          |
                  Qt thread +   eBUS SDK   eBUS SDK   TCP CoLa-B client
                  SsnRx (TCP)   (GVSP)     (GVSP)     + SPSC rings
                  + SBF write   + chunk    + flush    + parse thread
                    thread        queue +    thread   + writer thread
                                  writer
                         |          |          |          |
                     SBF files  jai-raw-seg ENVI BIL  scan_*.h5
                                 segments   + trig log
```

Every driver app derives from `common::IDriverApp` (`common/include/driver_app.h`):
`bool Init(external_stop)` / `void Run()` / `void Shutdown()`, a sticky
`HasFailed()`, `MicrosSinceLastData()` for the no-data watchdog and a shared
`TerminateFlag()`. `main.cpp` initialises the enabled drivers **sequentially**
(AsteRx first — its warm-up gate holds the whole rig until the receiver has GPS
time), runs each `Run()` on its own thread, polls all terminate flags every
100 ms together with the rig-wide guards, and propagates the first termination to
everyone (orderly join + shutdown in reverse order).

**A failed run is visible from the outside.** `main` exits `0` on a clean or
signal-interrupted run and `1` when a driver ended the run itself (or the
configuration could not even be snapshotted), and `drivers.json` in the session
folder records `completed`, `interrupted (signal N)`, `failed (<driver>)`,
`failed (disk)` or `failed (configuration)`. Without that, a rig that died thirty
seconds into a two-hour session was indistinguishable from one that finished.

### Targets and libraries

| Target | Description |
|--------|-------------|
| `AmigaDrivers` | The unified executable for multi-sensor recording |
| `ebus_discover` / `ebus_set_ip` | GigE Vision enumeration for both camera drivers / camera re-addressing (FORCEIP + persistent IP); both live in `common/` and are used by the web GUI |
| `jai_snapshot` / `fx10_snapshot` | One-shot Go-X frame grab / FX10 waterfall preview grab (web GUI). GenICam node names for `features.raw` are looked up with eBUS Player on real hardware (see `fx10_driver/docs/DEVICE_CONFIG.md`, "Discovering node names") |
| `fx10_reference` | Collect Reference page: white then dark, each for `reference.duration_s` (default 5 s), with separate spectral plots and a persistent `reference_<UTC>` session using the FX10 ENVI recorder. See [reference workflow](fx10_driver/docs/DEVICE_CONFIG.md#collect-reference-page). |
| `asterx_lib`, `fx10_lib`, `gox_lib`, `lms4xxx_lib` | Per-driver static libraries |
| `amiga_common` | Shared infrastructure: logging (file + non-blocking console sink), config schema helpers, signal handling, SPSC ring buffer, bounded queue, rotating file writer, rig guards, `drivers.json`, GUI marker contract |
| `amiga_ebus` | The eBUS-SDK-dependent layer shared by gox and fx10 (`common/include/ebus`): GenICam env bootstrap, PvResult errors, discovery, device IP configuration |

### Dependencies

| Dependency | Provided by | Used by |
|-----------|-------------|---------|
| HDF5 1.14.6 (C library, static `hdf5-static`) | FetchContent, pinned in `3rd_party/FetchContent/` (`cmake/Dependencies.cmake`) | lms4xxx scan recorder (`lms4xxx-h5`, see `docs/FORMAT_H5.md`) |
| zlib (`zlib1g-dev`) | system (devcontainer apt) | HDF5 gzip filter — a hard requirement of the pinned HDF5 CMake |
| spdlog v1.17.0 (+fmt) | FetchContent, pinned in `3rd_party/FetchContent/` | all (single process-wide logger) |
| yaml-cpp 0.9.0 | FetchContent, pinned in `3rd_party/FetchContent/` | main + all four driver configs |
| nlohmann/json v3.12.0 | FetchContent, pinned in `3rd_party/FetchContent/` | gox (`jai-raw-seg` `idx.jsonl`, device sidecars, snapshot tools), asterx/fx10 sidecars, `drivers.json` |
| doctest 2.4.11 | vendored `3rd_party/doctest/` | every unit-test suite (common + the four drivers) |
| Boost (header-only) | system | lms4xxx (Asio TCP) |
| Qt5 Core/Network/SerialPort | system | asterx (vendored Septentrio SsnRx SDK) |
| eBUS SDK (Pleora) 6.5.1 | installed in the devcontainer (single SDK, root `cmake/FindeBUS.cmake`) | gox + fx10 (GigE Vision) |

## Quick start

### Prerequisites

The C++ side builds inside the `amiga-drivers-dev` devcontainer
(`.devcontainer/`, repo mounted at `/workspace`), which brings the eBUS SDK,
Qt5, Boost and zlib. `Build.bash` installs the SDK `.deb` from `resource/` if it
is missing.

### Build

```bash
docker exec -w /workspace amiga-drivers-dev bash -lc \
  'cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)'
```

Binaries land in `build/bin/`, the test binaries in `build/bin/tests/`. Unit
tests build in every configuration (`AMIGA_BUILD_TESTS`, default `ON`) — see
[Development](#development).

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
    Operator: <Dev>
    Field: <Test>
    Output Directory: ./data
    Enable ASTERX: true            # the GUI's Enable switches write these
    Enable FX10: false
    Enable GOX: true
    Enable LMS4XXX: false
    ASTERX Driver Config Path: ./asterx_driver/config/config-asterx.yaml
    FX10 Driver Config Path: ./fx10_driver/config/config-fx10.yaml
    GOX Driver Config Path: ./gox_driver/config/config-gox.yaml
    LMS4XXX Driver Config Path: ./lms4xxx_driver/config/config-lms4xxx.yaml
Guards:                            # rig-wide, see Health guards below
    Disk Min Free GiB: 5           # stop the whole run below this (0 = off)
    Disk Warn Free GiB: 20
    No Data Warn S: 15
    No Data Abort S: 60            # any sensor silent this long stops the run (0 = off)
```

The `Guards:` values are range-checked at load, and a warning threshold that
could never fire before its stop threshold is rejected rather than accepted and
ignored — a guard that is silently misconfigured is worse than no guard, because
the log still says it is armed.

Each driver config is a heavily commented YAML template next to its driver. The
templates are **part of the documentation** — every non-obvious value carries the
manual page it comes from — so the GUI edits them as text rather than
re-serialising them. The shipped defaults keep every time source on
(`ntp.enabled: true`, `ptp.enabled: true`); the FX10 `sensor_trigger` block is
enabled per rig once the SensorSync board is wired.

**One schema policy for all four drivers** (`common/include/config_util.h`):
unknown keys are startup errors that name the key and list the accepted set,
omitted keys keep the driver defaults, every error carries the dotted key path,
and every numeric key is range-checked (a negative value into an unsigned slot is
refused by name, never wrapped). The templates share one layout — banner,
`device:`/`cameras:`/`lidar:`, acquisition, driver extras, `output:`, `network:`,
`logging:` — one comment style (short trailing notes: unit, range, allowed
values) and one unit-suffix rule (`_s`, `_ms`, `_hz`, `_mb`, `_bytes`, `_deg`).
Each driver's tests load its shipped template(s), so template rot fails the build.

## Session output

Each run creates one session folder; everything the run produces sits under
`raw/`:

```
<Output Directory>/<YYYYMMDD_HHMMSS>/
└── raw/
    ├── log_<ts>.log                  # unified trace-level log (the GUI's primary feed)
    ├── drivers.json                  # run header, versions, per-driver final statistics, final status
    ├── config/                       # snapshot of config-main + every enabled driver's config
    ├── asterx/                       # asterx-<UTC>-N.sbf + live_*.csv; prewarm/ holds the
    │                                 #   SBF context streamed before the warm-up gate opened
    ├── gox/<cam>/                    # seg_NNNNN.raw (jai-raw-seg) + seg_NNNNN.idx.jsonl + segments.jsonl
    │                                 #   + device.json (one-shot audit) + telemetry.jsonl + stream_stats.txt
    ├── fx10/fx10_<UTC>Z/             # capture.json + segment_NNNN.bil/.hdr/.lines.csv + sensor_trigger.log
    │                                #   + device.json + telemetry.jsonl + stream_stats.txt + segments.jsonl
    └── lms4xxx/                      # scan_<instance>_<ts>_NNN.h5
```

The recording formats are self-describing enough to read without this repository:

- **SBF** (asterx) — the receiver's own binary format, block for block as it
  arrived (CRC-checked by the SDK, never re-serialised); RxTools / `sbf2rin`
  read it directly. Blocks streamed before the warm-up gate opened live in
  `prewarm/`, never in the accepted files.
- **`jai-raw-seg`** (gox) — 12-bit packed Bayer payloads verbatim from the SDK
  buffer behind a 96-byte CRC-protected frame header; `gox_driver/scripts/`
  `inspect_raw.py` / `unpack_raw.py` are the reference readers, and the index
  can be rebuilt from the headers alone.
- **ENVI BIL** (fx10) — a segment is valid **iff its `.hdr` exists**; the header
  is written last, and it carries the read-back exposure and frame rate, the AIE
  state and which calibration pack applies. `.lines.csv` gives every line its
  BlockID, raw camera tick and any gap/rejection event.
- **`lms4xxx-h5`** (lms4xxx) — `/frames/*` per-scan metadata, `/channels/*` raw
  `[scan, point]` values with their scale factors, and `/telemetry/*` device
  health. HDF5 has no atomic rename, so a finished file is marked instead: **a
  file without the `closed_cleanly` root attribute is incomplete.** Flushes are
  `H5Fflush` + `fdatasync`, so the loss window really is `flush_interval_ms`.
  Scans are only written once the device clock is NTP-locked
  (`docs/DEVICE_CONFIG.md`, "Time"). `lms4xxx_driver/scripts/inspect_h5.py` is
  the reference reader (`info` / `verify` / `csv`).

Every driver writes from a thread that is not the one talking to the device
(AsteRx and LMS writer threads, the Go-X chunk queue + writer, the FX10 flush
thread), so a stalling disk shows up as queue depth in the statistics — and as a
fail-fast stop once a bounded budget is exhausted — never as a sensor that
silently lost data because its socket was not being read.

## Health guards

Recording silently producing nothing is the failure mode that matters. Two of
those rules are rig-wide and live in `main.cpp`, driven by the `Guards:` block
of `config/config-main.yaml`:

| Guard | What it watches | Default |
|---|---|---|
| Disk floor | Free space (GiB, `f_bavail`) on the filesystem holding the session folder — checked once before bring-up and once a second afterwards | warn below 20, stop below 5 |
| No-data watchdog | `IDriverApp::MicrosSinceLastData()` per driver: how long that sensor has produced nothing | warn after 15 s, stop after 60 s |

They are rig-wide because every driver writes under
`<Output Directory>/<timestamp>/raw/` (one filesystem, one number), and because
one dead sensor already makes the session incomplete. The abort default is 60 s
rather than something snappier because AsteRx repairs a 30 s SBF silence *before
recording* (warm-up) by reconnecting: a rig-wide abort at 30 s would race that.
Once recording, that same silence timer is fatal (fail-fast). A driver that is
*legitimately* quiet — an external trigger with no pulses, a receiver warm-up —
reports `nullopt` and is simply not watched while that lasts; deciding *that* is
the one part the drivers keep, because only they know it. A watchdog trip is
logged as `<Driver> driver stopped: no data (...)`, which is what turns that
sensor's GUI card red.

On top of the two, each driver keeps the guards that are specific to its device:

| Driver | Guards |
|---|---|
| asterx | Warm-up gate on `ReceiverStatus` up-time / FINETIME before any block is recorded; a 30 s SBF silence timer that **reconnects before recording** and is **fatal while recording**; damaged blocks, link loss, a receiver reset and a full write queue while recording are fatal too (fail-fast) |
| gox | PTP slave-status / clock-accuracy guard, thermal warning, `Counter0` missed-trigger accounting; the first dropped / lost / incomplete frame is fatal (fail-fast) |
| fx10 | *Stream-unusable* abort — buffers keep arriving but none is usable, which total silence cannot detect and main therefore cannot see — thermal limits (processing board 80 °C / FPGA 90 °C), recorder-failure classification, SensorSync log integrity and stall; the first lost / unrecorded frame or missed trigger is fatal (fail-fast) |
| lms4xxx | First-telegram content verification, NTP server probe, NTP time lock and device-time step check (no host clock involved), consecutive framing-error threshold, writer failure; the first lost / damaged scan is fatal (fail-fast) |

Any of these ends the whole rig's run — and says so in the exit status
(`drivers.json` records `failed (<driver>)`, or `failed (disk)`).

The two snapshot tools (`fx10_snapshot`, `jai_snapshot`) do not run under
`main`, so `Guards:` never applies to them; each keeps its own 10 s no-frame
bound so a GUI preview of a dead camera fails fast instead of waiting out its
capture budget.

## Logging conventions

Every line is `[HH:MM:SS] [level] [Module]: msg`. The session log file is the
complete record; the console copy on stderr is best-effort: when stderr is a
pipe (the GUI, `docker exec`) the console sink is non-blocking and drops lines
rather than letting a stalled reader block a driver thread, and the final log
lines report how many were dropped. On top of that, the drivers follow four rules.

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
[LMS4xxxApp]: [Statistics] [Front_Center_Laser] up=00:00:10  rate=600.0 Hz  fps=598.0  ntp=OK  frames=6000  ...  unexpected=0  prelock=0  tstep_max_us=812
[AsteRx]:     [Statistics] blocks=48210  bytes=12.4 MB  files=1  crc_fail=0  length_errors=0  discarded_bytes=0  ...  queue_pending=0  queue_max=4096
```

`fps=` is a GUI contract: `app/services/driver_stats.py` parses it into the
dashboard's per-sensor cards, and `tools/check_contracts.py` fails if either side
renames it. AsteRx records a byte stream rather than frames, so it has no `fps=`;
during the receiver warm-up its line carries `warmup=<up>/<min>  finetime=0|1`
instead. The lms4xxx `ntp=` token is `OFF`, `NO-LOCK` (streaming, device clock
not yet plausible), `OK`, `NO-TS` or `UNREACH`.

**4. Throwing is an app-layer decision.** `common::DriverLog` never throws by
default; the explicit `g_log.Error(true, ...)` overload (log, then
`std::runtime_error` with the formatted message) is the only sanctioned throw in
driver code and is reserved for `*_driver_app.cpp`. Lower layers propagate
failures upward instead — `std::error_code` returns (lms4xxx) or driver-internal
exception types (`RecorderError`, `TransportError`, `SdkError`, …) that the app
layer catches — and the app layer decides whether to abort.
(`common::Log::LogAndThrow` remains, but only `main.cpp` and `common/` use it,
and `main.cpp` turns every start-up failure into a logged exit code 1.)

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
this project's log rate) behind reality. The GUI reads the session log *file*;
the process's stderr pipe only feeds it until that file exists.

## Development

### Tests

Unit tests build in every configuration (`-DAMIGA_BUILD_TESTS=OFF` to skip them):

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
recorders, writer threads and the on-disk formats are exercised against synthetic
input, most of it built from the vendor manuals' own field tables rather than
from the implementation. The asterx session tests speak the real ASCII protocol
to a fake receiver on localhost through the vendored SsnRx parser; the common
tests fill a real pipe to prove the console sink never blocks.

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
├── main.cpp                  # unified entry point: session folder, guards, threads, exit status
├── CMakeLists.txt            # top-level build (AMIGA_BUILD_TESTS, version + git SHA)
├── Build.bash / Start.bash   # convenience wrappers (SDK install + build, setcap + run)
├── .devcontainer/            # amiga-drivers-dev image + compose (host network, SYS_NICE)
├── cmake/                    # Dependencies.cmake (hdf5/spdlog/yaml-cpp/nlohmann) + FindeBUS.cmake
├── 3rd_party/                # pinned FetchContent sources + vendored doctest
├── config/config-main.yaml   # driver selection, guards, output root
├── resource/                 # eBUS SDK .deb + devices_ip.yaml (rig addresses, time server)
├── common/                   # amiga_common + amiga_ebus + tests/
├── asterx_driver/            # Qt/SsnRx session, SBF write queue, live CSV sidecar, tests/
├── fx10_driver/              # fx10_core (SDK-free) + fx10_ebus (eBUS glue) + tools/ + tests/
├── gox_driver/               # jai_core (SDK-free) + jai_ebus (eBUS glue) + scripts/ + tools/ + tests/
├── lms4xxx_driver/           # CoLa B driver, scan parser, HDF5 recorder, scripts/, tests/
├── submodule/sensor_trigger/ # Teensy SensorSync-Logger firmware + host client (fx10 timing)
├── app/                      # NiceGUI web GUI (host-side, uv-managed .venv)
├── tools/                    # repo tooling (contract checker, …)
└── AGENTS.md                 # review protocol for automated reviewers
```

## Documentation

| Document | Covers |
|---|---|
| [`asterx_driver/docs/DEVICE_CONFIG.md`](asterx_driver/docs/DEVICE_CONFIG.md) | Reset to `RxDefault` on every connect, account handling, the warm-up gate, the write queue and the fail-fast rules while recording |
| [`gox_driver/docs/DEVICE_CONFIG.md`](gox_driver/docs/DEVICE_CONFIG.md) | `UserSetLoad Default` on every bring-up, the ordered apply plan, the raw-feature audit, PTP (grandmaster = AsteRx, L2-domain prerequisite), fail-fast and the on-disk residue after a crash |
| [`fx10_driver/docs/DEVICE_CONFIG.md`](fx10_driver/docs/DEVICE_CONFIG.md) | Factory user set + calibration-ROI guard, the write-order plan, which parameters are exposed and why the rest stay at their factory values, the recording policy (time source, fail-fast, off-thread durability) |
| [`lms4xxx_driver/docs/DEVICE_CONFIG.md`](lms4xxx_driver/docs/DEVICE_CONFIG.md) | `mSCloadappdef` baseline, every telegram with its page number, `sAN` status-byte polarity, the shutdown handshake, the device self-report, "Time": NTP routing prerequisite, time lock and step check |
| [`lms4xxx_driver/docs/FORMAT_H5.md`](lms4xxx_driver/docs/FORMAT_H5.md) | The `lms4xxx-h5` layout, durability and completeness semantics, what the timestamps mean |
| [`lms4xxx_driver/docs/sopas_filter_polarity.md`](lms4xxx_driver/docs/sopas_filter_polarity.md) | Why two filter status bytes contradict the manual's tables |
| [`resource/devices_ip.yaml`](resource/devices_ip.yaml) | Rig addresses, host NIC per device, the AsteRx as NTP/PTP server |
| [`submodule/sensor_trigger/README.md`](submodule/sensor_trigger/README.md) | SensorSync-Logger protocol, session log format and offline post-processing |
| [`TODO.md`](TODO.md) | Roadmap and the log of completed milestones |

The device-config documents are the ones to read before changing anything that
talks to hardware: they record not just what the driver writes, but what it
deliberately does **not** write, and why — with the manual page for each decision.
