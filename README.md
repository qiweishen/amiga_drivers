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
The user-confirmed PTP topology has AsteRx as its sole grandmaster; the Go-X
driver does not verify the master's identity. Go-X retains slave-status
admission and runtime guards. It does not read `GevIEEE1588ClockAccuracy` or
use that grandmaster capability node as a slave synchronization quality
criterion; the existing `ptp.accuracy` output key remains `null` for format
compatibility (not sampled, not an indication of degraded synchronization).

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
  <img alt="GUI"      src="https://img.shields.io/badge/GUI-NiceGUI%20%C2%B7%20pip-3776AB?logo=python&logoColor=white">
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
                      |     AmigaDrivers: composition + coordinator       |
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
`TerminateFlag()`. `common::RunSession` initializes the enabled drivers **sequentially**
(AsteRx first — its warm-up gate holds the whole rig until the receiver has GPS
time), runs each `Run()` on its own thread, polls all terminate flags every
100 ms together with the rig-wide guards, and propagates the first termination to
everyone (orderly join + shutdown in reverse order).

**Run results are explicit.** After shutdown, the coordinator evaluates driver failures,
rig guards and status persistence before assigning the final result. A clean or
signal-interrupted run exits `0`; a reported failure exits `1`. The session's
`raw/drivers.json` records `completed`, `interrupted (signal N)` or a failure
reason, together with final per-driver statistics. A missing final manifest is
an unknown/incomplete result, even if the process has disappeared.

Lifecycle ownership is explicit:

- `main.cpp`: signals, main configuration and the process error exit.
- `acquisition.cpp`: output/session creation, config snapshots and driver construction.
- `common::RunSession`: sequential initialization, concurrent Run, guards, reverse
  shutdown and final manifest. `Init` may start resources; even failed/partial
  initialization must be followed by `Shutdown`. Every Run thread joins before
  Shutdown drains internal workers/writers. An instance represents one epoch.
- `fx10::Session`: device/receiver/recorder lifetime and counters;
  `Fx10DriverApp` only adapts the common interface.
- LMS `CommandChannel`: serialized CoLa transactions and uncertainty;
  configuration, streaming and lifecycle have separate source files.
- `AcquisitionController` in `app/services/process.py`: process identity,
  tasks, stop latch and immutable recording snapshots; UI code observes state.

The preview readers also have explicit boundaries: `app/formats/fx10.py`
defines FX10 ENVI/capture/index rules, while `gox_driver/scripts/jai_raw/`
defines Go-X record layouts and pixel decoding for both CLI and GUI.
Services choose full, tail or streamed reads and render previews. The on-disk
recording formats and existing CLI names remain unchanged.

The existing Go-X RGB8 preview path still passes RGB arrays to OpenCV image
writers that interpret channels as BGR. That display limitation is unchanged by
this refactor; native sample accounting and raw recording bytes are unaffected.

### GUI and control service

The default deployment runs NiceGUI and the C++ acquisition program in one
Ubuntu 22.04 container. NiceGUI's local control service manages the C++ process:

```text
Browser → amiga-drivers-dev container
             └─ NiceGUI (app.main, Python 3.10)
                  └─ local control service → AmigaDrivers / device tools (C++)
                           shared configuration, logs and recordings
```

The control service owns process identities, recording/tool reservations,
configuration writes and request recovery. Preview and historical replay read recorded files;
they do not replace the C++ recording path. Acquisition binaries must publish the
`amiga-run-v1` lifecycle protocol. Logs provide diagnostics and display statistics;
they do not establish readiness or a successful recording result. The supported
deployment is the single container shown above. See [Web GUI](#web-gui) for recovery.

### Targets and libraries

| Target | Description |
|--------|-------------|
| `AmigaDrivers` | The unified executable for multi-sensor recording |
| `ebus_discover` / `ebus_set_ip` | GigE Vision enumeration for both camera drivers / camera re-addressing (FORCEIP + persistent IP); both live in `common/` and are used by the web GUI |
| `jai_snapshot` / `fx10_snapshot` | One-shot Go-X frame grab / FX10 waterfall preview grab (web GUI). GenICam node names for `features.raw` are looked up with eBUS Player on real hardware (see `fx10_driver/docs/DEVICE_CONFIG.md`, "Discovering node names") |
| `fx10_reference` | Collect Reference page: white then dark, each for `reference.duration_s` (default 5 s), with separate spectral plots and a persistent `reference_<UTC>` session using the FX10 ENVI recorder. See [reference workflow](fx10_driver/docs/DEVICE_CONFIG.md#collect-reference-page). |
| `asterx_lib`, `fx10_lib`, `gox_lib`, `lms4xxx_lib` | Per-driver static libraries |
| `amiga_common` | SDK-independent infrastructure: session coordinator, logging, strict config loading, queues, rig guards, SensorSync host wrapper and `drivers.json` |
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
| eBUS SDK (Pleora) 6.5.1-6797 | exact local package and SHA-256 checked by the Dockerfile; shared `cmake/FindeBUS.cmake` | gox + fx10 hardware adapters |
| SensorSync host client | reviewed local source snapshot, four consumed files pinned by SHA-256 in `cmake/SensorTriggerSnapshot.cmake`; configure never updates it | shared camera trigger session |

The GUI/control service use Python 3.10+ and the direct dependency versions in
[`requirements.txt`](requirements.txt): NiceGUI, PyYAML, NumPy and OpenCV.
The C++ acquisition program does not need Python to record data. These are direct
version pins, not a transitive Python lock or a fully hermetic OS image.

Build boundaries are explicit (all default to `ON`):
`AMIGA_ENABLE_EBUS` controls hardware camera adapters and device tools, while
`jai_core`, `fx10_core` and their tests remain available with it off.
`AMIGA_ENABLE_ASTERX` controls the Qt/ssnrx driver;
`AMIGA_ENABLE_LMS4XXX` controls the LiDAR driver and HDF5/zlib dependency.
A configuration enabling an excluded driver is rejected before initialization.
`AMIGA_BUILD_TESTS=OFF` also omits contract-test registration.
`AMIGA_BUILD_REVISION` supplies the recorded source identity (default `unknown`);
configure does not query Git. The existing manifest field name `git_sha` is retained.
SensorSync is pinned by local content, not an inferred upstream commit; missing or
changed files stop configuration and require an explicit reviewed snapshot update.

## Quick start

### Prerequisites

The C++ side builds inside the `amiga-drivers-dev` devcontainer
(`.devcontainer/`, repo mounted at `/workspace`), which brings the eBUS SDK,
Qt5, Boost and zlib. The Dockerfile installs the pinned SDK package from `resource/`.

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

The devcontainer Compose starts one container for the GUI and acquisition tools.
From the repository root, after normally stopping any active recording and
closing an older host-side GUI:

```bash
docker compose -f .devcontainer/docker-compose.yml up -d --build --remove-orphans
```

Open <http://localhost:8619>. Subsequent starts can omit `--build` when the image
inputs have not changed. `--remove-orphans` removes the former separate `gui`
container when migrating from the two-container configuration. No controller
URL or shared API token is needed in this integrated setup. Building the image
prepares Python dependencies and the SDK environment; it does not compile the
acquisition binaries. See [Docker deployment](#docker-deployment) for mounts,
logs and stop/restart behavior.

GUI startup does not start a recording. See [Web GUI](#web-gui) for controls and
[Docker deployment](#docker-deployment) for deployment details.

## Configuration

The executable accepts one main configuration path, defaulting to
`config/config-main.yaml`. It selects the drivers, their per-driver config paths
and the output root. The GUI can select that file through
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

The main configuration requires a `General:` mapping and accepts only the
`General`, `Guards`, and `Sensor Trigger` sections and their documented keys.
An omitted device switch defaults to `false` for all four drivers, including
LMS4xxx. Present but malformed values are startup errors, never a request to use
a default. Paths must be nonblank; `Sensor Trigger.Port: ""` deliberately disables
the board. Older unused sections such as `Logging System` are rejected.

`Guards:` values must be finite and within their allowed ranges. Warning and
stop thresholds are checked together. Low disk space at preflight produces a
failed manifest and exit code 1 before driver initialization; failure to persist
that result is reported separately.

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
process ownership. All Enable switches and Camera Tools' **Apply to config**
actions use the same local configuration service.

Deployments can constrain resolved configuration paths with `AMIGA_CONFIG_ROOT`
and recording output with `AMIGA_DATA_ROOT`. The single container shares these
paths between the GUI and acquisition tools.

**One schema policy for the main config and all four drivers** (`common/include/config_util.h`):
unknown keys are startup errors that name the key and list the accepted set,
omitted keys keep the driver defaults, every error carries the dotted key path,
and floating-point values must be finite before range checks or conversion to
durations. Numeric fields enforce their declared bounds; a negative value into
an unsigned slot is refused by name, never wrapped. The driver templates share one layout — banner,
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
    ├── log_<ts>.log                  # diagnostics, live rates and diagnostic lifecycle messages
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
<AMIGA_RUNTIME_DIR>/                  # default: app/_runtime
├── acquisition.json                 # current acquisition status rendezvous (amiga-run-v1)
├── tool-operation.json              # latest device tool identity and outcome
├── controller.lock                  # shared control ownership lock (unless overridden)
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
those rules are rig-wide and live in `common/src/session_runner.cpp`, driven by the `Guards:` block
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
failure reason. GUI lifecycle state comes exclusively from the structured
status protocol.

On top of the two, each driver keeps the guards that are specific to its device:

| Driver | Guards |
|---|---|
| asterx | Warm-up gate on `ReceiverStatus` up-time / FINETIME before any block is recorded; a 30 s SBF silence timer that **reconnects before recording** and is **fatal while recording**; damaged blocks, link loss, a receiver reset and a full write queue while recording are fatal too (fail-fast) |
| gox | PTP slave-status admission / runtime guard, thermal warning, `Counter0` missed-trigger accounting, SensorSync log integrity / stall / session-start guards (silence under SensorSync pulses is watched like freerun); the first dropped / lost / incomplete frame is fatal (fail-fast) |
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

**1. Module tags identify ownership.** Driver/session messages use
`AsteRxApp`, `FX10App`, `GoXApp` or `LMS4xxxApp`; lower layers use their
internal driver/subsystem tags. These names group diagnostics; log severity
never determines lifecycle state.

**2. Message prefixes identify the instance, then the subsystem.**
Multi-instance drivers tag lines with the camera/LiDAR ID, followed where useful
by `[Writer]`, `[eBUS]`, `[TCP]`, `[Live]` or `[TriggerLog]`.

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
[LMS4xxxApp]: [Statistics] [Front_Center_Laser] up=00:00:10  rate=600.0 Hz  fps=598.0  ntp=UNVERIFIED  frames=6000  ...  unexpected=0  prelock=0  tstep_max_us=812
[AsteRx]:     [Statistics] blocks=48210  bytes=12.4 MB  files=1  crc_fail=0  length_errors=0  discarded_bytes=0  ...  queue_pending=0  queue_max=4096
```

`fps=` is a GUI contract: `app/services/driver_stats.py` parses it into the
dashboard's per-sensor cards, and `tools/check_contracts.py` fails if either side
renames it. AsteRx records a byte stream rather than frames, so it has no `fps=`;
during the receiver warm-up its line carries `warmup=<up>/<min>  finetime=0|1`
instead. The lms4xxx `ntp=` token is `OFF`, `NO-LOCK` (streaming, device clock
not yet plausible), `UNVERIFIED`, `UNKNOWN` (device warning status stale/unknown),
`NO-SIGNAL` (device NTP warning), `TIME-ANOMALY`, `NO-TS` or `UNREACH`.
Host server reachability is a separate field; no token certifies absolute time accuracy.

**4. Failures cross a defined boundary.** Lower layers return their native
error codes or exception types. Device/session adapters latch integrity failures;
the coordinator joins Run workers and shuts down every app before finalizing
the manifest. Diagnostic lifecycle messages remain readable but are no longer
a GUI protocol.

## Web GUI

`app/` is a NiceGUI control panel (port 8619). The default Compose configuration
runs it alongside the C++ binaries in one container.
Its local control service owns acquisition, device tools and configuration writes.
See [Docker deployment](#docker-deployment) for the single-container configuration.

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

Acquisition binaries must publish `status_schema: amiga-run-v1` in
`raw/drivers.json`, including process identity (`pid`, `start_ticks`, `boot_id`),
session location and lifecycle transitions:
`initializing → running → stopping → finished`. The controller passes
`AMIGA_STATUS_FILE` to the acquisition process to publish the same document at
a known recovery location. GUI readiness and configuration locks follow this
protocol after identity/schema checks; runtime `fps=` continues to come from the
statistics log.

Recovery checks process and session identity rather than assuming the newest
folder belongs to the running process. Binaries without this manifest are
unsupported and cannot establish readiness or a successful result. Invalid
structured state cannot silently be treated as proof of readiness. An ended manifest, an explicit
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
coordinate independently launched command-line device tools. A browser reconnect
can recover the active session without starting another recording. Closing or
refreshing the browser does not stop acquisition. Stopping or restarting the
default container requests acquisition shutdown; a new container start does not
automatically begin another recording.

### Single-container settings

The supported deployment is `amiga-drivers-dev`: `python -B -m app.main`
starts prepared C++ binaries directly in the same container. Docker exec/auto
backends and the standalone HTTP controller have been retired. Remove the old
`AMIGA_GUI_MODE` and `AMIGA_CONTROLLER_*` settings when migrating.
Do not run a second control service against the same devices.

| Setting | Default / purpose |
| --- | --- |
| `AMIGA_GUI_HOST`, `AMIGA_GUI_PORT` | GUI and Compose default to `0.0.0.0:8619`, accepting connections through localhost, LAN and Tailscale IPv4 addresses. Set `AMIGA_GUI_HOST` to restrict the listening address. |
| `AMIGA_MAIN_CONFIG` | Default `<repo>/config/config-main.yaml`; selects the actual per-driver config paths. |
| `AMIGA_SNAPSHOT_CONFIG` | Default `<repo>/gox_driver/config/config-gox-snapshot.yaml`. |
| `AMIGA_CONFIG_ROOT`, `AMIGA_DATA_ROOT` | Optional resolved configuration/output boundaries; Compose sets both to `/workspace`. |
| `AMIGA_RUNTIME_DIR` | Default `<repo>/app/_runtime`; operation journals and temporary previews; Compose keeps them on the workspace volume. |
| `AMIGA_CONTROL_LOCK` | Default `<AMIGA_RUNTIME_DIR>/controller.lock`; exclusive local control ownership lock. |

GUI service shutdown always requests tool/acquisition stop and waits for cleanup.
Browser disconnects do not trigger service shutdown. Preview decoding uses the
shared in-process format libraries; no separate decoder interpreter is selected.

**Refresh.** Log/statistics updates depend on the driver's log flush, `LOG_POLL_S` and the page refresh cadence
in `app/constants.py`. These are display intervals, not sampling rates or timing
accuracy guarantees. The GUI reads the session log file; stderr is a temporary
feed until that file is available.

## Docker deployment

### Standard workstation setup

Use [`.devcontainer/docker-compose.yml`](.devcontainer/docker-compose.yml) for
the single `amiga-drivers-dev` service. It includes the C++/SDK development
environment, acquisition tools and NiceGUI. Its entry point is
`/usr/bin/python3 -B -m app.main`, managing C++ processes locally. The GUI defaults to `0.0.0.0:8619` with Linux host networking;
`AMIGA_GUI_HOST` selects another listening address. No controller URL, API port
or shared API token is needed.

[`.devcontainer/Dockerfile`](.devcontainer/Dockerfile) has one Ubuntu 22.04
image stage, retaining the `drivers` target name. APT supplies Python 3.10 and
pip, and Python's patch version follows Ubuntu package updates. The bundled
Pleora eBUS 6.5.1 package targets the same Ubuntu release and retains its
`/opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64` installation path. The application's
C++ drivers use the SDK's acquisition core; the vendor Python binding is not
used by the application.

On an operator-initiated image build, pip installs the four pinned direct
dependencies from [`requirements.txt`](requirements.txt). pip resolves their
transitive dependencies for Python 3.10. This single file is copied into
`/opt/amiga-python`, outside the repository bind mount. Keep its direct versions
reviewed together when updating them. There is no UV step, virtual
environment, constraints file or Python source build. Container startup does not
install dependencies, and image builds do not compile the project's acquisition
binaries.

The container uses the existing repository, configuration and data mounts.
The default UID/GID remains `1000:1000`; `AMIGA_UID`/`AMIGA_GID` can override the
image build inputs. The mounted repository and recording directories must be
writable by that identity. Runtime journals remain under `app/_runtime`.

Before rebuilding, recreating or stopping the container, normally stop any
active recording in the GUI and wait for its final result. The following are
operator commands, not steps performed by this static review:

```bash
docker compose -f .devcontainer/docker-compose.yml up -d --build --remove-orphans
docker compose -f .devcontainer/docker-compose.yml logs --tail=100 -f amiga-drivers-dev
docker compose -f .devcontainer/docker-compose.yml restart amiga-drivers-dev
docker compose -f .devcontainer/docker-compose.yml stop amiga-drivers-dev
```

`--remove-orphans` removes the old `gui` service container when migrating from the
previous configuration. The workstation setup retains the existing shared disk
at `/mnt/SharedData/Post_Processing_Data`, X11 socket and authorization file,
user runtime and device mounts. These host paths must exist; the X11 bind mounts
are configured to fail instead of creating missing source paths. The active
desktop session's `DISPLAY` and `XAUTHORITY` values select GUI forwarding.

`build/bin/` acquisition/tool binaries must be prepared separately. Startup does
not start recording, and the GUI can report missing binaries before acquisition
is available. IDE integration starts only `amiga-drivers-dev` and preserves the
GUI entry point.

Closing the browser leaves recording running. Stopping or restarting the unified
container shuts down the GUI service, requests tool/acquisition stop and waits
for cleanup. Compose grants 180 seconds before Docker may force termination;
the actual maximum drain time has not been measured. The next startup does not
automatically resume recording. Health checks inspect GUI readiness at
`/healthz`; they do not connect to sensors or certify recording health.

**Validation boundary:** these container changes have been inspected as text.
Image builds, dependency installation, C++ compilation, service startup and
hardware behavior have not been executed or verified here.

### Remote GUI access over Tailscale

The GUI now defaults to `0.0.0.0:8619`, accepting connections through the server's
localhost, LAN and Tailscale IPv4 addresses. No Tailscale-specific bind setting is
required. An existing `AMIGA_GUI_HOST` environment value takes precedence over
this default. With Linux host networking, Docker port mappings are not used.
See [Docker host networking](https://docs.docker.com/engine/network/drivers/host/).

Optionally, to restrict GUI access to the Tailscale interface, first normally stop
any active recording. Then run from the remote Linux server's project root with
Tailscale already connected:

```bash
export AMIGA_GUI_HOST="$(tailscale ip -4)"
# Check that this is the remote server's Tailscale IPv4 address:
printf '%s\n' "$AMIGA_GUI_HOST"
docker compose -f .devcontainer/docker-compose.yml up -d --no-build --force-recreate amiga-drivers-dev
```

Open `http://<remote-server-tailscale-ip>:8619/` from your other Tailscale device.
The GUI binds specifically to that address; use the same address for server-side
curl checks instead of `localhost`. The health check runs
[`.devcontainer/gui_healthcheck.py`](.devcontainer/gui_healthcheck.py), follows the
selected bind address and requires `/healthz` to report `ready: true`.
This recreates the unified GUI/acquisition container without building the image;
it does not preserve an active recording across the restart.
The [Tailscale CLI](https://tailscale.com/docs/reference/tailscale-cli) documents
`tailscale ip -4`; [Compose up](https://docs.docker.com/reference/cli/docker/compose/up/)
documents the service recreation flags.

Keep this setting for future Compose invocations. For persistence, place
`AMIGA_GUI_HOST=<remote-server-tailscale-ip>` in a local `.devcontainer/.env` file
and use `docker compose --env-file .devcontainer/.env -f .devcontainer/docker-compose.yml ...`.
An invocation without the setting reverts to the all-interface default when it
recreates the container. Tailscale must have assigned the address before GUI startup.

Setting `AMIGA_GUI_HOST=0.0.0.0` explicitly restores the default on all IPv4
interfaces. The GUI has no browser login; select the listening scope appropriate
to the deployment.

If access still fails, compare a server-side request to its Tailscale IP with a
request from the client. If the server succeeds but the client fails, inspect
Tailscale connectivity/access policy and the host firewall for TCP 8619. These
remote network checks have not been performed from this workspace.

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

A focused checker covers the remaining statistics-display contract:

```bash
python -B tools/check_contracts.py
```

It verifies the `[Statistics]` `fps=` field and module/instance routing of every
frame-based driver, and that the GUI's parser reads a rendered line of each.
`common_tests` additionally asserts the C++ log output matches the GUI's line
regex, and that an error line reaches the file without an explicit flush.

This is a suggested check after changes to `[Statistics]` format strings; it does
not validate the structured lifecycle protocol.
The build/test commands above describe operator workflows and are not evidence
that they were executed for these container changes. Automated review sessions
must respect the execution limits in [`AGENTS.md`](AGENTS.md).

## Repository layout

```
amiga_drivers/
├── main.cpp                  # process entry: signals, main config, error exit
├── acquisition.cpp           # session folder, config snapshots, driver composition
├── CMakeLists.txt            # build boundaries, tests and caller-supplied revision
├── Start.bash               # convenience wrapper for setcap + command-line recording
├── .devcontainer/            # one Ubuntu image, GUI/acquisition Compose service and IDE settings
├── cmake/                    # Dependencies.cmake (hdf5/spdlog/yaml-cpp/nlohmann) + FindeBUS.cmake
├── 3rd_party/                # FetchContent sources, External/sensor_trigger + vendored doctest
├── config/config-main.yaml   # driver selection, guards, output root
├── resource/                 # eBUS SDK .deb + devices_ip.yaml (rig addresses, time server)
├── common/                   # amiga_common + amiga_ebus + tests/
├── asterx_driver/            # Qt/SsnRx session, SBF write queue, live CSV sidecar, tests/
├── fx10_driver/              # fx10_core (SDK-free) + fx10_ebus (eBUS glue) + tools/ + tests/
├── gox_driver/               # jai_core (SDK-free) + jai_ebus (eBUS glue) + scripts/ + tools/ + tests/
├── lms4xxx_driver/           # CoLa B driver, scan parser, HDF5 recorder, scripts/, tests/
├── app/                      # Python GUI/control services (pip-managed dependencies)
│   ├── main.py               # NiceGUI single-container entry point
│   ├── formats/              # shared FX10 recording-format readers
│   ├── services/             # process/tool lifecycle, config actions, status, file previews and replay
│   └── ui/                   # Overview, Config, Logs, Live, AsteRx, History, Camera Tools, Reference
├── tools/                    # repo tooling (contract checker, …)
├── requirements.txt          # pinned direct Python dependencies; pip resolves transitives
└── AGENTS.md                 # review protocol for automated reviewers
```

## Documentation

| Document | Covers |
|---|---|
| [`.devcontainer/Dockerfile`](.devcontainer/Dockerfile) | Ubuntu 22.04, eBUS SDK, C++ tools and system Python 3.10 with pip dependencies |
| [`.devcontainer/docker-compose.yml`](.devcontainer/docker-compose.yml) | One GUI/acquisition service, shared paths, host networking, device access and shutdown grace period |
| [`.devcontainer/devcontainer.json`](.devcontainer/devcontainer.json) | IDE attachment to the same running service |
| [`asterx_driver/docs/DEVICE_CONFIG.md`](asterx_driver/docs/DEVICE_CONFIG.md) | Reset to `RxDefault` on every connect, account handling, the warm-up gate, the write queue and the fail-fast rules while recording |
| [`gox_driver/docs/DEVICE_CONFIG.md`](gox_driver/docs/DEVICE_CONFIG.md) | `UserSetLoad Default` on every bring-up, the ordered apply plan, the raw-feature audit, PTP slave-status guards (user-confirmed sole grandmaster = AsteRx, identity not verified by the driver, L2-domain prerequisite), fail-fast and the on-disk residue after a crash |
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
