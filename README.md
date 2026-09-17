# Amiga Drivers

**Unified data acquisition for a four-sensor mobile mapping rig.** One process,
one log, one session folder — a GNSS/INS receiver, two GigE Vision cameras and a
set of 2D LiDARs recording concurrently behind a shared logging, configuration
and lifecycle framework, with a web control panel on top.

**Time.** The rig is configured around **AsteRx GPS time**: LMS4xxx uses NTP,
Go-X uses PTP plus SensorSync hardware triggering (Line5 pulse in, Line2
ExposureActive out), and FX10 uses SensorSync strobe edges against AsteRx
PPS/ZDA. The two cameras share the one SensorSync board through a single
session per run (`common::SensorSyncHub`, log at `raw/sensor_trigger.log`).
Actual receiver timescales, the ZDA-to-GPS conversion, wiring and time lock must
be established for the deployed rig; configuration alone does not demonstrate
cross-sensor synchronization. Native device timestamps remain distinct from
recorded `host_*` diagnostics. AsteRx CSV history playback uses `host_unix_ns`
for its display timeline, not as a replacement for GNSS/INS measurement time.
Host monotonic clocks also drive operational deadlines, rates and playback.
FX10 recordings without SensorSync lack that cross-sensor timing association.

**Fail-fast.** All four apps share `Init → Run → Shutdown` and a sticky
`HasFailed()` result. Reported driver failures and rig-wide guards request an
orderly stop of the whole recording. These checks cover conditions such as
damaged input, link loss, incomplete frames, counter gaps and queue overflow;
their presence is not proof that every loss is detectable. Shutdown drains
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
- [Logging conventions](#logging-conventions) · [Web GUI](#web-gui) · [Docker deployment](#docker-deployment)
- [Development](#development) · [Repository layout](#repository-layout) · [Documentation](#documentation)

## Sensors

| Sensor | Driver | Transport | Records |
|---|---|---|---|
| Septentrio **AsteRx RBi3 Pro+** (GNSS/INS; the rig's NTP + PTP server) | `asterx_driver` | Qt + vendored SsnRx SDK over TCP | `*.sbf` blocks (+ `prewarm/` context) + `live_*.csv` telemetry sidecar |
| JAI **Go-X** GigE cameras (×N) | `gox_driver` | Pleora eBUS SDK (GVSP), PTP slave, SensorSync-triggered | `jai-raw-seg` segments + `idx.jsonl` + `device.json` + `telemetry.jsonl` per camera; its channel in `raw/sensor_trigger.log` |
| Specim **FX10e** hyperspectral pushbroom | `fx10_driver` | Pleora eBUS SDK (GVSP), SensorSync-triggered | ENVI BIL `.bil`/`.hdr` + `.lines.csv`; its channel in `raw/sensor_trigger.log` |
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

**Run results are explicit.** After shutdown, `main` evaluates driver failures,
rig guards and status persistence before assigning the final result. A clean or
signal-interrupted run exits `0`; a reported failure exits `1`. The session's
`raw/drivers.json` records `completed`, `interrupted (signal N)` or a failure
reason, together with final per-driver statistics. A missing final manifest is
an unknown/incomplete result, even if the process has disappeared.

### GUI and control service

The NiceGUI interface and the acquisition controller can run together for local
development or as separate services for deployment:

```text
Browser → NiceGUI (app.main)
             ├─ integrated: local control service → native / devcontainer tools
             └─ remote: HTTP API → app.controller → AmigaDrivers / device tools
                         GUI ← read-only shared config, logs and recordings
```

The controller owns process identities, recording/tool reservations, configuration
writes and request recovery. Preview and historical replay read recorded files;
they do not replace the C++ recording path. New acquisition binaries publish the
`amiga-run-v1` lifecycle protocol, while older binaries use labelled log
compatibility mode. See [Web GUI](#web-gui) for operating modes and recovery.

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

The GUI/controller use Python 3.12+ and the existing dependencies declared in
[`pyproject.toml`](pyproject.toml): NiceGUI, PyYAML, NumPy and OpenCV. The controller
uses FastAPI/Starlette/uvicorn supplied by the NiceGUI environment. The combined
devcontainer Dockerfile prepares this runtime during image builds; the optional
`deploy/` packaging recipes instead require an already prepared runtime image.

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
> to normal scheduling with a warning rather than failing. Provision required
> capabilities in the deployment environment; the web GUI does not run `setcap`.

### Web control panel

The existing devcontainer Compose now starts both the driver/controller container
and a separate GUI container. From the repository root, after ending any active
recording and closing an older host-side GUI:

```bash
docker compose -f .devcontainer/docker-compose.yml up -d --build
```

Open <http://localhost:8619>. Subsequent starts can omit `--build` when the image
inputs have not changed. The controller creates its shared credential on first
startup; no separate `deploy/.env`, manual token or copied configuration tree is
needed. See [the two-container guide](.devcontainer/README.md) for prerequisites,
logs and stop/restart commands. Building images prepares Python dependencies and
the SDK environment; it does not compile the acquisition binaries.

For a separate native setup with no running controller, the existing project
environment can still run the GUI directly:

```bash
AMIGA_GUI_MODE=native AMIGA_GUI_HOST=127.0.0.1 .venv/bin/python -B -m app.main
```

GUI/controller startup does not start a recording. See [Web GUI](#web-gui) for
operating modes and [Docker deployment](#docker-deployment) for deployment details.

## Configuration

The executable accepts one main configuration path, defaulting to
`config/config-main.yaml`. It selects the drivers, their per-driver config paths
and the output root. The GUI/controller can select that file through
`AMIGA_MAIN_CONFIG`:

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
Sensor Trigger:                    # the rig's one SensorSync board, shared by FX10 and Go-X
    Port: "/dev/serial/by-id/usb-Teensyduino_USB_Serial_16838390-if00"   # "" = no board
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
(`ntp.enabled: true`, `ptp.enabled: true`, and `sensor_trigger.enabled: true`
with `trigger.mode: external` in both camera configs). The SensorSync board's
serial port lives in `config-main.yaml` only (`Sensor Trigger: Port`); the camera
configs declare just their channel and pulse rate, and `main` logs one
`SensorSync: ...` line summarising what the rig asked of the board.

The GUI resolves driver editors from the paths in the selected main config.
Saving checks both the resolved path and file modification time, so a stale page
cannot silently overwrite another edit. Configuration changes apply at the next
recording start; they do not reconfigure an active recording. Writes are blocked
during acquisition initialization/stopping, device tool operations, or uncertain
process ownership. In remote mode all GUI writes go through the controller,
including Enable switches and Camera Tools' **Apply to config** actions.

Deployments can constrain resolved configuration paths with `AMIGA_CONFIG_ROOT`
and recording output with `AMIGA_DATA_ROOT`. The two services must see shared
configurations and recordings at the same absolute paths.

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

Each acquisition run creates one session folder containing its recordings,
configuration snapshot and run metadata under `raw/`:

```
<Output Directory>/<YYYYMMDD_HHMMSS>/
└── raw/
    ├── log_<ts>.log                  # diagnostics, live rates and legacy GUI health markers
    ├── drivers.json                  # process/session identity, lifecycle, versions and final results
    ├── config/                       # snapshot of config-main + every enabled driver's config
    ├── sensor_trigger.log            # SensorSync timing log, one session per rig: trigger (T) and
    │                                 #   exposure (S) edges of every channel, PPS (P) and NMEA (Z)
    ├── asterx/                       # asterx-<UTC>-N.sbf + live_*.csv; prewarm/ holds the
    │                                 #   SBF context streamed before the warm-up gate opened
    ├── gox/<cam>/                    # seg_NNNNN.raw (jai-raw-seg) + seg_NNNNN.idx.jsonl + segments.jsonl
    │                                 #   + device.json (one-shot audit, timing block) + telemetry.jsonl + stream_stats.txt
    ├── fx10/fx10_<UTC>Z/             # capture.json + segment_NNNN.bil/.hdr/.lines.csv
    │                                #   + device.json + telemetry.jsonl + stream_stats.txt + segments.jsonl
    └── lms4xxx/                      # scan_<instance>_<ts>_NNN.h5
```

Controller journals and temporary previews are separate from acquisition data:

```text
<AMIGA_RUNTIME_DIR>/                  # default: app/_runtime; production controller: /control
├── acquisition.json                 # current acquisition status rendezvous (amiga-run-v1)
├── tool-operation.json              # latest device tool identity and outcome
├── controller.lock                  # shared control ownership lock (unless overridden)
├── controller-token                 # auto-created credential in the devcontainer setup only
├── requests/                        # bounded standalone-controller request journals
├── snapshot/                        # Go-X preview work files
└── snapshot_fx10/                    # FX10 preview work files
```

`raw/drivers.json` remains the per-session record. The runtime status file is a
recovery aid for locating the current session, not a history archive. FX10
reference collection uses separate `reference_<UTC>` output sessions. Historical
AsteRx replay reads the CSV sidecars without modifying any of these files.

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
logged as `<Driver> driver stopped: no data (...)` and appears in the final run
failure reason. New binaries supply GUI lifecycle state through the structured
status protocol; older binaries use log markers to update sensor cards.

On top of the two, each driver keeps the guards that are specific to its device:

| Driver | Guards |
|---|---|
| asterx | Warm-up gate on `ReceiverStatus` up-time / FINETIME before any block is recorded; a 30 s SBF silence timer that **reconnects before recording** and is **fatal while recording**; damaged blocks, link loss, a receiver reset and a full write queue while recording are fatal too (fail-fast) |
| gox | PTP slave-status / clock-accuracy guard, thermal warning, `Counter0` missed-trigger accounting, SensorSync log integrity / stall / session-start guards (silence under SensorSync pulses is watched like freerun); the first dropped / lost / incomplete frame is fatal (fail-fast) |
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
`driver_markers.h`). In legacy log compatibility mode, these are the lines the
GUI health state machine reacts to (an App-level `error` marks the sensor FAILED).
New `amiga-run-v1` runs use structured lifecycle state instead. Every other file in a driver logs
under the short internal module (`AsteRx` / `FX10` / `GoX` / `LMS4xxx`), which
the GUI displays but never routes: a transient internal warning must not flip a
sensor's health. Shared `common/` components use neutral modules
(`DriversJson`) — never a driver's name.

**2. Message prefixes identify the instance, then the subsystem.**
Multi-instance drivers tag every line with the instance first: `[cam0] ...` (gox
cameras), `[Front_Center_Laser] ...` (lms instances). On App-level `error` lines
this leading tag routes legacy GUI failures to that instance (no tag = all
instances). Subsystem files add a fixed second-level prefix after the
instance tag: `[Writer]` (segment/file writers), `[eBUS]` (Pleora SDK
control/stream code), `[TCP]` (socket transport). Special-purpose sidecars keep
their own tag in the same style (`[Live]` asterx CSV feed, `[TriggerLog]` fx10
SensorSync session). Example:
`[LMS4xxx]: [Front_Center_Laser] [Writer] Recording to ...`.

**3. Statistics are uniform across drivers.** Periodic status lines start with
`[Statistics]` (plus the instance tag where applicable), use double-space-separated
`key={}` fields, and report two measured rates over the same window — `rate=N.N Hz`,
the sensor's own output rate, and `fps=N.N`, the frames written by the recorder.
A rate difference can reflect buffering or loss; use final counters and recording
metadata to assess the outcome. New fields are appended, never
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
successfully`, …) remain verbatim contracts for legacy GUI compatibility.
The structured lifecycle protocol does not depend on their wording; live `fps=`
still comes from the statistics log. For lms4xxx the per-instance
markers come from each instance app and the driver-level pair is aggregated by
`main.cpp` once all instances are up / down.

## Web GUI

`app/` is a NiceGUI control panel (port 8619). It supports integrated native/docker
control and a remote GUI mode using `AMIGA_CONTROLLER_URL`. In remote mode a
single `app.controller` service owns acquisition, device tools and configuration
writes; the GUI reads shared recordings without writing them. Production packaging,
configuration for the standard two-service setup are in
[.devcontainer/README.md](.devcontainer/README.md); the optional standalone
production recipe and its acceptance plan remain in [deploy/README.md](deploy/README.md).

### Pages and workflows

| Page | What it does |
| --- | --- |
| `/` **Overview** | Start/stop a recording; per-driver Enable switches for the next start; sensor health, storage and written-frame rates; final run results and recent command outcomes. |
| `/config` **Config** | Raw-text editor for the selected main and driver configs; YAML validation, path/version conflict detection, atomic save and a `.bak`. |
| `/logs` **Logs** | The unified session log with level/module filters and follow mode. |
| `/live` **Data Live** | On-demand Go-X/FX10 previews from files in the RUNNING session, using bounded background work and the session/settings captured at request time. |
| `/asterx` **AsteRx** | Live CSV telemetry plus independent historical replay, seek timeline, playback speed, indexing progress and stream-gap indicators. |
| `/sessions` **History** | Bounded session metadata listing, date/device/state filters, pagination and links to finalized AsteRx recordings. |
| `/reference` **Collect Reference** | FX10 white/dark reference collection with saved configuration and retained results. |
| `/camera` **Camera Tools** | One shared device discovery for both camera drivers, then a Go-X and an FX10 exposure workbench. |

### AsteRx history playback

1. Open **History** (`/sessions`) and select a recording root. Filter by date,
   enabled device or recorded state; the list displays 20 sessions per page.
2. Use the AsteRx replay action on a finalized, inactive session containing live
   CSV data. This opens `/asterx` in **History** mode with that session selected.
3. Alternatively, enter a path directly on the AsteRx page: a session directory,
   `raw/`, `raw/asterx/`, or a supported CSV file. Paths refer to the GUI server's
   filesystem/shared volume, not the browser's computer. Direct entry also works
   for relocated or older recordings outside the catalog.
4. Load the file(s), then use **Play/Pause**, **Restart**, the seek slider or
   **Jump to**, and playback speeds from **0.25× to 8×**. Indexing shows byte
   progress and can be cancelled.

Supported streams are `live_insnavgeod.csv` and `live_receiverstatus.csv`;
selecting a directory combines both when present. Each page has an independent
player. It uses the recorded integer `host_unix_ns` values to order the display;
the live receiver and active acquisition remain independent of playback. These
CSV files contain derived telemetry, so this feature does **not** decode full
SBF recordings or replay raw GNSS/IMU observations to a device.

Use files that have finished writing. Indexing/playback checks for file changes,
reports malformed rows and incomplete tails, and rejects backwards host time
for automatic playback. Gaps longer than 2 seconds are counted per stream, with
up to 512 intervals retained for display. Sparse indexing and a bounded route
preview limit memory use; original CSV values and files are not rewritten.

History reads top-level timestamped directories and small `raw/drivers.json`
files, without traversing raw sensor payloads. Scans are bounded to 100,000
directory entries, 5,000 candidate sessions, 256 KiB per manifest and 64 MiB of
metadata; the page reports when a listing is partial. Catalog states describe
the recorded manifest result, not an independent integrity audit of every file.

### Camera tools and reference collection

**Camera Tools** covers what the Go-X and FX10 pages used to do separately. One
Scan runs `ebus_discover --json` into a single table — every GigE Vision camera on
the host's adapters, classified by vendor (JAI → Go-X, Specim → FX10) with its
subnet mask, IP-configuration state and subnet validity; picking a row targets
that driver's workbench. Three rules shape the page:

- *The GigE control channel is exclusive.* Discovery is a broadcast the cameras
  answer without it, so scanning is allowed during a verified recording while
  other tools are idle. It is blocked during startup, stopping or uncertain
  ownership; a camera owned by the running recording is marked "in use" and its
  snapshot / Set IP buttons stay disabled.
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

**Collect Reference** runs the FX10 white/dark workflow with saved settings,
separate spectral plots and retained results. Discovery, Set IP, both snapshot
tools and reference collection share service-owned operation tracking. Leaving
a page does not make its device operation available for a second launch.
The shared header displays the tool ID, status/error and **Stop tool** action.

### Lifecycle, results and recovery

New acquisition binaries publish `status_schema: amiga-run-v1` in
`raw/drivers.json`, including process identity (`pid`, `start_ticks`, `boot_id`),
session location and lifecycle transitions:
`initializing → running → stopping → finished`. The controller passes
`AMIGA_STATUS_FILE` to the acquisition process to publish the same document at
a known recovery location. GUI readiness and configuration locks follow this
protocol after identity/schema checks; runtime `fps=` continues to come from the
statistics log.

Recovery checks process and session identity rather than assuming the newest
folder belongs to the running process. Older binaries remain supported through
explicitly labelled log compatibility mode. Invalid structured state cannot
silently be treated as proof of readiness. An ended manifest, an explicit
`recording_failed: false` and a recognized completed/interrupted result are
needed for a successful run result; an attached process must also exit cleanly.
Overview exposes the final run metadata and per-driver counters.

Tool operations journal their process identity before the tool is released to
execute. Timeout or **Stop tool** first sends TERM; any tool-only forced stop
targets that recorded identity and verifies exit. Acquisition receives only TERM
so it can drain and close. If exit/ownership cannot be confirmed, conflicting
controls remain locked and the uncertainty is shown to the operator.

Only one control service should operate a device environment. All such services
must use the same `AMIGA_CONTROL_LOCK` file. This application lock does not
coordinate independently launched command-line device tools. A GUI reconnect or
restart can recover the active session without starting another recording;
restarting the standalone controller never automatically starts a new session.

### Operating modes and settings

| Mode | Selection | Control owner |
| --- | --- | --- |
| Integrated native | `AMIGA_GUI_MODE=native`, no controller URL | GUI service starts prepared binaries in `build/bin/` locally. |
| Integrated devcontainer | `AMIGA_GUI_MODE=docker`, no controller URL | GUI service executes tools in `amiga-drivers-dev`; shared paths use `app/constants.py`'s mount map. |
| Integrated auto | No controller URL; `AMIGA_GUI_MODE=auto` (default) | Detects the devcontainer, otherwise uses native execution. |
| Remote GUI | Set `AMIGA_CONTROLLER_URL` | A separate `app.controller` owns acquisition/tools/config writes; the GUI does not probe Docker or launch local device commands. |

The combined devcontainer Compose uses remote GUI mode, with `app.controller`
inside `amiga-drivers-dev`. Integrated modes are alternatives for environments
without a running controller, not additional controllers for this stack. Start
the services through Compose; the old GUI **Start container** action is removed.

Both Python entry points run from the repository root in an existing environment:
`.venv/bin/python -B -m app.main` and
`.venv/bin/python -B -m app.controller`. Before starting a standalone controller,
configure its token, backend and shared paths. A remote GUI requires the matching
token and a reachable controller URL; the URL takes precedence over GUI backend
selection. Do not set `AMIGA_CONTROLLER_URL` on the controller itself.

| Setting | Default / purpose |
| --- | --- |
| `AMIGA_GUI_HOST`, `AMIGA_GUI_PORT` | GUI and combined Compose default to `0.0.0.0:8619`, accepting connections through localhost, LAN and Tailscale IPv4 addresses. Set `AMIGA_GUI_HOST` to restrict the listening address. |
| `AMIGA_CONTROLLER_HOST`, `AMIGA_CONTROLLER_PORT` | Standalone API bind: `127.0.0.1:8620`; production Compose overrides the host binding. |
| `AMIGA_CONTROLLER_URL` | Unset: integrated mode. Set to the controller's base HTTP URL for remote mode. |
| `AMIGA_CONTROLLER_TOKEN_FILE` | Existing shared secret file for controller and remote GUI; preferred over `AMIGA_CONTROLLER_TOKEN`. Token: 32–4096 printable ASCII characters without spaces. |
| `AMIGA_CONTROLLER_BOOTSTRAP_TOKEN` | Controller-only opt-in (`1`) to create a missing token file atomically with mode `0600`; enabled by the combined devcontainer Compose. Existing credentials are never replaced. |
| `AMIGA_MAIN_CONFIG` | Default `<repo>/config/config-main.yaml`; selects the actual per-driver config paths. |
| `AMIGA_SNAPSHOT_CONFIG` | Default `<repo>/gox_driver/config/config-gox-snapshot.yaml`. |
| `AMIGA_CONFIG_ROOT`, `AMIGA_DATA_ROOT` | Optional resolved configuration/output boundaries; production uses `/config` and `/workspace/dataset`. |
| `AMIGA_RUNTIME_DIR` | Default `<repo>/app/_runtime`; operation journals and temporary previews. Use persistent storage for the standalone controller. |
| `AMIGA_CONTROL_LOCK` | Default `<AMIGA_RUNTIME_DIR>/controller.lock`; host and container controllers must share the same underlying file for a common device environment. |
| `AMIGA_PYTHON` | Preview decoder interpreter; default `<repo>/.venv/bin/python`, production default `/opt/venv/bin/python`. |

### Controller API

The internal HTTP protocol is `amiga-control-v1`. The GUI keeps its Bearer token
on the server; it is not sent to the browser. All `/v1` routes require it.

| Endpoint | Purpose |
| --- | --- |
| `GET /healthz` | Public controller readiness; does not probe hardware or certify recording health. |
| `GET /v1/state` | Controller/session state, runtime statistics, reference results and recent command summaries. |
| `POST /v1/jobs` | Submit a fixed recording, camera, reference, tool-stop or configuration action with a request ID. |
| `GET /v1/jobs/{id}` | Read the accepted/running/completed/failed request and retained result. |

Commands carry a controller identity, and Stop also checks the session generation
to reject a stale page's request. Request IDs are journaled before execution;
while retained, the same ID/content is deduplicated and changed content with that
ID is rejected. At most 32 requests are retained, with results bounded to 8 MiB.
An unfinished request from an earlier controller instance reports an unknown
outcome and is not automatically replayed. Evicted requests return 404; this is
not permanent exactly-once delivery.

On a connection error, keep the displayed request ID and inspect Overview's
recent commands or the retained result before deciding what to do next. A lost
response does not prove that the hardware command failed. The GUI does not
automatically resubmit device commands. The API exposes fixed actions rather
than arbitrary shell execution, and raw recordings stay on the shared volume.

**Refresh.** Remote GUI state polling runs every second. Log/statistics updates
also depend on the driver's log flush, `LOG_POLL_S` and the page refresh cadence
in `app/constants.py`. These are display intervals, not sampling rates or timing
accuracy guarantees. The GUI reads the session log file; stderr is a temporary
feed until that file is available.

## Docker deployment

### Standard workstation setup

Use [`.devcontainer/docker-compose.yml`](.devcontainer/docker-compose.yml) for
both services. `amiga-drivers-dev` retains the existing C++/SDK development
environment and runs the controller API; `gui` runs NiceGUI with read-only
project/data mounts. Both use the existing project configuration and shared
disk, and communicate over `127.0.0.1:8620` using Linux host networking. The GUI
defaults to `0.0.0.0:8619`; `AMIGA_GUI_HOST` selects another listening address.
Only the driver container retains device access.

One Dockerfile provides separate `drivers` and `gui` targets sharing a Python
dependency layer. On an operator-initiated build it prepares `/opt/venv` from
the existing `uv.lock`, outside the repository bind mount; startup never syncs
the host `.venv`. The controller creates a missing shared token under
`app/_runtime` with mode `0600`, and both containers use the same UID/GID.
The default remains `1000:1000`; `AMIGA_UID`/`AMIGA_GID` can override build inputs.

```bash
docker compose -f .devcontainer/docker-compose.yml up -d --build
docker compose -f .devcontainer/docker-compose.yml logs --tail=100 -f gui amiga-drivers-dev
# Restart only the GUI, leaving controller-managed recording running:
docker compose -f .devcontainer/docker-compose.yml restart gui
```

This retains the workstation's existing shared-disk, X11 authorization and
device mount prerequisites. `build/bin/` acquisition/tool binaries must be
prepared separately. Container startup itself does not start recording, and
the GUI can report missing binaries before acquisition is available. IDE
integration starts both services and preserves the controller command.
See [.devcontainer/README.md](.devcontainer/README.md) for details and the static
validation boundary; these images/services have not been built or run here.

### Remote GUI access over Tailscale

The GUI now defaults to `0.0.0.0:8619`, accepting connections through the server's
localhost, LAN and Tailscale IPv4 addresses. No Tailscale-specific bind setting is
required. An existing `AMIGA_GUI_HOST` environment value takes precedence over
this default. With Linux host networking, Docker port mappings are not used.
See [Docker host networking](https://docs.docker.com/engine/network/drivers/host/).

Optionally, to restrict GUI access to the Tailscale interface, run from the
remote Linux server's project root with Tailscale already connected:

```bash
export AMIGA_GUI_HOST="$(tailscale ip -4)"
# Check that this is the remote server's Tailscale IPv4 address:
printf '%s\n' "$AMIGA_GUI_HOST"
docker compose -f .devcontainer/docker-compose.yml up -d --no-build --no-deps --force-recreate gui
```

Open `http://<remote-server-tailscale-ip>:8619/` from your other Tailscale device.
The GUI binds specifically to that address; use the same address for server-side
curl checks instead of `localhost`. The health check runs
[`.devcontainer/gui_healthcheck.py`](.devcontainer/gui_healthcheck.py), follows the
selected bind address and requires `/healthz` to report `ready: true`, while
controller communication remains at `127.0.0.1:8620`. Only the GUI
container is recreated; no image build or controller restart is needed.
The [Tailscale CLI](https://tailscale.com/docs/reference/tailscale-cli) documents
`tailscale ip -4`; [Compose up](https://docs.docker.com/reference/cli/docker/compose/up/)
documents the service recreation flags.

Keep this setting for future Compose invocations. For persistence, place
`AMIGA_GUI_HOST=<remote-server-tailscale-ip>` in a local `.devcontainer/.env` file
and use `docker compose --env-file .devcontainer/.env -f .devcontainer/docker-compose.yml ...`.
An invocation without the setting reverts to the all-interface default when it
recreates the GUI. Tailscale must have assigned the address before GUI startup.

Setting `AMIGA_GUI_HOST=0.0.0.0` explicitly restores the default on all IPv4
interfaces. The GUI has no browser login, and its server-side API token does not
authenticate browser users; select the listening scope appropriate to the deployment.

If access still fails, compare a server-side request to its Tailscale IP with a
request from the client. If the server succeeds but the client fails, inspect
Tailscale connectivity/access policy and the host firewall for TCP 8619. Do not
change the controller's 8620 binding to solve a GUI connectivity issue. These
remote network checks have not been performed from this workspace.

### Optional standalone production recipe

The separate [`deploy/`](deploy/README.md) recipe is retained for deployment
from prebuilt images and independent configuration directories. It is an
alternative to the workstation setup; do not run both against the same devices.
It also targets one Linux host with two services:

| Service | Responsibilities and mounts |
| --- | --- |
| `controller` | Runs acquisition/device tools and writes configs; host network; read/write `/config`, `/workspace/dataset` and persistent `/control`. |
| `gui` | NiceGUI, log viewing and file previews/history; read-only `/config` and `/workspace/dataset`; no Docker socket or device-node mounts. |

Both services use an explicit deployment UID/GID and read-only container root
filesystems. The GUI publishes to host `127.0.0.1:8619` by default. The controller
listens on host port 8620 with Bearer authentication, and the GUI reaches it via
the host gateway. This is a single-host internal API design. Remote browser
access needs an appropriate authenticated/TLS entry point; the GUI does not
provide a user login system.

Deployment inputs and files:

- [`deploy/.env.example`](deploy/.env.example): existing image names, actual
  UID/GID, prepared configuration/data/control directories and a private token
  file. Bind source directories must already exist with suitable permissions.
- [`deploy/config-main.example.yaml`](deploy/config-main.example.yaml): shared
  container paths, with all four device types initially disabled. Supply the
  actual device YAMLs and snapshot configuration under `/config`.
- [`deploy/compose.production.yaml`](deploy/compose.production.yaml): two
  services using existing images only (`pull_policy: never`, no build step).
- [`deploy/gui.Dockerfile`](deploy/gui.Dockerfile) and
  [`deploy/controller.Dockerfile`](deploy/controller.Dockerfile): packaging
  recipes with no dependency installation or compilation. They require prepared
  runtime base images; the controller also requires the six named binaries in
  `build/bin/` and ABI/SDK-compatible libraries. A freshly prepared acquisition
  binary containing the status-protocol changes is needed for `amiga-run-v1`.
- [`deploy/compose.serial.yaml`](deploy/compose.serial.yaml): optional overlay
  for a specific verified host serial device/group, exposed as `/dev/ttyAMIGA`.

The production controller requests `NET_RAW`, `NET_ADMIN` and `SYS_NICE`;
effective capabilities, SDK paths, network behavior and serial access still
require validation under the chosen runtime UID. The GUI Start action does not
change executable capabilities or prepare the host environment.

Restarting the GUI leaves controller-managed acquisition running. Stopping the
controller rejects new commands and requests orderly tool/acquisition shutdown.
Compose allows 180 seconds for controller shutdown; the actual maximum drain
time is unverified, and Docker may force termination after that deadline.
Controller startup/restart does not automatically begin recording.

**Validation status:** GUI-12–15 were checked statically (43 app Python files
parsed as AST, three deployment YAML files parsed with duplicate-key checks).
Those checks did not execute project modules. C++ compilation, tests, service
startup, container builds and hardware acceptance have not been performed for
these changes. See the [deployment acceptance plan](deploy/README.md#尚未执行的验收方案)
for the remaining process-recovery, API, history and container scenarios.

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
.venv/bin/python -B tools/check_contracts.py
```

It verifies the lifecycle markers (`common/include/driver_markers.h` ↔
`app/services/markers.py`), the `[Statistics]` `fps=` field of every frame-based
driver, and that the GUI's parser really reads a rendered line of each.
`common_tests` additionally asserts the C++ log output matches the GUI's line
regex, and that an error line reaches the file without an explicit flush.

This is a suggested check after changes to marker or `[Statistics]` format
strings; it does not validate the HTTP API or structured lifecycle protocol.
The build/test commands above describe operator workflows and are not evidence
that they were executed for GUI-12–15. Automated review sessions must respect
the execution limits in [`AGENTS.md`](AGENTS.md).

## Repository layout

```
amiga_drivers/
├── main.cpp                  # unified entry point: session folder, guards, threads, exit status
├── CMakeLists.txt            # top-level build (AMIGA_BUILD_TESTS, version + git SHA)
├── Build.bash / Start.bash   # convenience wrappers (SDK install + build, setcap + run)
├── .devcontainer/            # drivers + GUI image targets, shared Compose and workstation guide
├── deploy/                   # separate GUI/controller packaging, Compose, config examples, acceptance plan
├── cmake/                    # Dependencies.cmake (hdf5/spdlog/yaml-cpp/nlohmann) + FindeBUS.cmake
├── 3rd_party/                # FetchContent sources, External/sensor_trigger + vendored doctest
├── config/config-main.yaml   # driver selection, guards, output root
├── resource/                 # eBUS SDK .deb + devices_ip.yaml (rig addresses, time server)
├── common/                   # amiga_common + amiga_ebus + tests/
├── asterx_driver/            # Qt/SsnRx session, SBF write queue, live CSV sidecar, tests/
├── fx10_driver/              # fx10_core (SDK-free) + fx10_ebus (eBUS glue) + tools/ + tests/
├── gox_driver/               # jai_core (SDK-free) + jai_ebus (eBUS glue) + scripts/ + tools/ + tests/
├── lms4xxx_driver/           # CoLa B driver, scan parser, HDF5 recorder, scripts/, tests/
├── app/                      # Python GUI/control services (existing uv-managed .venv)
│   ├── main.py               # NiceGUI entry point: integrated or remote mode
│   ├── controller.py         # authenticated HTTP control service
│   ├── services/             # process/tool lifecycle, config actions, status, file previews and replay
│   └── ui/                   # Overview, Config, Logs, Live, AsteRx, History, Camera Tools, Reference
├── tools/                    # repo tooling (contract checker, …)
└── AGENTS.md                 # review protocol for automated reviewers
```

## Documentation

| Document | Covers |
|---|---|
| [`.devcontainer/README.md`](.devcontainer/README.md) | Single Compose startup for driver/controller + GUI containers, shared paths, automatic credentials and validation limits |
| [`deploy/README.md`](deploy/README.md) | GUI-12–15 implementation, controller API/recovery, two-container deployment inputs and unexecuted acceptance scenarios |
| [`asterx_driver/docs/DEVICE_CONFIG.md`](asterx_driver/docs/DEVICE_CONFIG.md) | Reset to `RxDefault` on every connect, account handling, the warm-up gate, the write queue and the fail-fast rules while recording |
| [`gox_driver/docs/DEVICE_CONFIG.md`](gox_driver/docs/DEVICE_CONFIG.md) | `UserSetLoad Default` on every bring-up, the ordered apply plan, the raw-feature audit, PTP (grandmaster = AsteRx, L2-domain prerequisite), fail-fast and the on-disk residue after a crash |
| [`fx10_driver/docs/DEVICE_CONFIG.md`](fx10_driver/docs/DEVICE_CONFIG.md) | Factory user set + calibration-ROI guard, the write-order plan, which parameters are exposed and why the rest stay at their factory values, the recording policy (time source, fail-fast, off-thread durability) |
| [`lms4xxx_driver/docs/DEVICE_CONFIG.md`](lms4xxx_driver/docs/DEVICE_CONFIG.md) | `mSCloadappdef` baseline, every telegram with its page number, `sAN` status-byte polarity, the shutdown handshake, the device self-report, "Time": NTP routing prerequisite, time lock and step check |
| [`lms4xxx_driver/docs/FORMAT_H5.md`](lms4xxx_driver/docs/FORMAT_H5.md) | The `lms4xxx-h5` layout, durability and completeness semantics, what the timestamps mean |
| [`lms4xxx_driver/docs/sopas_filter_polarity.md`](lms4xxx_driver/docs/sopas_filter_polarity.md) | Why two filter status bytes contradict the manual's tables |
| [`resource/devices_ip.yaml`](resource/devices_ip.yaml) | Rig addresses, host NIC per device, the AsteRx as NTP/PTP server |
| [`3rd_party/External/sensor_trigger/README.md`](3rd_party/External/sensor_trigger/README.md) | SensorSync-Logger protocol, session log format and offline post-processing |
| [`TODO.md`](TODO.md) | Roadmap and the log of completed milestones |

The device-config documents are the ones to read before changing anything that
talks to hardware: they record not just what the driver writes, but what it
deliberately does **not** write, and why — with the manual page for each decision.
