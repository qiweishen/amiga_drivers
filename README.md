# Phenomate Drivers

PhenomateDrivers records the raw output of a four-sensor mobile mapping rig: a Septentrio AsteRx
GNSS/INS receiver, JAI Go-X RGB cameras, a Specim FX10e hyperspectral camera and SICK LMS4xxx 2D
LiDARs. One process, `PhenomateDrivers`, runs all enabled sensors in one session with shared
logging, configuration and lifecycle handling. A NiceGUI web panel in the same container starts
and stops recordings and provides camera tools and previews. Offline decoding, export and
calibration belong to the separate `PhenomateDecoders` project.

<p align="left">
  <img alt="C++20"    src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <img alt="CMake"    src="https://img.shields.io/badge/CMake-%E2%89%A5%203.18-064F8C?logo=cmake&logoColor=white">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Ubuntu%2022.04%20(devcontainer)-E95420?logo=ubuntu&logoColor=white">
  <img alt="GUI"      src="https://img.shields.io/badge/GUI-NiceGUI-3776AB?logo=python&logoColor=white">
</p>

## Contents

- [Sensors](#sensors) · [Time and synchronization](#time-and-synchronization) ·
  [Architecture](#architecture)
- [Build and test](#build-and-test) · [Running](#running) · [Configuration](#configuration) ·
  [Session output](#session-output)
- [Health guards](#health-guards) · [Logging](#logging) · [Web GUI](#web-gui) ·
  [Docker deployment](#docker-deployment)
- [Repository layout](#repository-layout) · [Documentation](#documentation) ·
  [Limitations](#limitations)

## Sensors

| Sensor | Driver | Interface | Recorded data |
|---|---|---|---|
| Septentrio AsteRx RBi3 Pro+ (GNSS/INS; rig NTP and PTP server) | `asterx_driver` | TCP, vendored SsnRx SDK (Qt5) | SBF blocks as delivered by the SDK; `live_*.csv` telemetry sidecars |
| JAI Go-X GigE cameras (×N) | `gox_driver`, `ebus_driver` | GigE Vision, eBUS SDK 6.5.1; SensorSync trigger; optional PTP slave | `ebus-raw` SDK buffers, `device.json`, `events.jsonl` |
| Specim FX10e hyperspectral line-scan camera | `fx10_driver`, `ebus_driver` | GigE Vision, eBUS SDK 6.5.1; SensorSync trigger | `ebus-raw` SDK buffers, `device.json`, `events.jsonl` |
| SICK LMS4xxx 2D LiDARs (LMS4121R/LMS4124R family, ×N) | `lms4xxx_driver` | CoLa B over TCP port 2111; device NTP client | Complete CoLa B telegrams (BIN), CSV index, JSON metadata |

On every start each driver brings its device to a defined baseline and then writes only the
parameters the recording depends on, reading them back where the device supports it:

| Device | Baseline command |
|---|---|
| AsteRx | `exeCopyConfigFile, Boot, Current` after login |
| Go-X | `UserSetSelector = Default`, `UserSetLoad` |
| FX10 | Factory preparation, then the camera's `.pvcfg` file (`device.ebus_config`) |
| LMS4xxx | `mSCloadappdef` |

No driver writes network or IP settings. The only IP writer is the operator-triggered
`ebus_set_ip` tool (*Camera Tools → Set IP*). The rig's addresses and time server are listed in
`resource/devices_ip.yaml`; device-level details are in each driver's
[`docs/DEVICE_CONFIG.md`](#documentation).

## Time and synchronization

The rig is configured around AsteRx GPS time. The AsteRx serves NTP and PTP. LMS4xxx clocks follow
NTP. Go-X cameras are triggered by the SensorSync board (Teensy 4.1) on `Line5` and report
`ExposureActive` on `Line2`; FX10 frames are triggered by the same board on `Line0`. The board's
log, `raw/sensor_trigger.log`, records trigger and exposure edges together with AsteRx PPS and NMEA
ZDA; both cameras share one board session per run (`common::SensorSyncHub`). Go-X cameras can also
run as PTP slaves (`ptp.enabled`, off in the shipped profiles); AsteRx is then the user-confirmed
sole grandmaster, whose identity the driver does not verify.

Native device timestamps are recorded unchanged. Host-side times (`host_*` fields, AsteRx CSV
`host_unix_ns`) are reception diagnostics, not measurement times, and host monotonic clocks drive
only deadlines and rate statistics. The run manifest does not establish cross-sensor association;
receiver timescales, wiring and lock state must be established for the deployed rig.

## Architecture

```text
PhenomateDrivers (main.cpp)
└─ common::RunSession
   ├─ AsterxDriverApp ─ SsnRx session → SBF write queue → SBF files
   ├─ EbusCameraDriver (FX10; one per Go-X camera)
   │     └─ PvBuffer → owned copy → bounded queue → RAW writer
   └─ Lms4xxxDriverApp (one per LiDAR) ─ CoLa B receiver → time monitor → RAW recorder
```

Every driver app implements `common::IDriverApp` (`common/include/driver_app.h`): `Init`, `Run`,
`Shutdown`, a sticky `HasFailed()`, `MicrosSinceLastData()` for the no-data watchdog, and
`TerminateFlag()`. A session proceeds as follows:

1. `main.cpp` loads the main configuration (relative paths resolve against the repository root),
   creates `<Output Directory>/<YYYYMMDD_HHMMSS>/raw/`, opens the session log and the SensorSync
   hub, and copies every configuration in use into `raw/config/`. An existing session folder is
   refused, never reused.
2. It sends `LMCstopmeas` to every LiDAR in the LMS configuration, including disabled entries and
   runs with LMS acquisition disabled; unreachable devices are skipped after the connection
   timeout ([details](lms4xxx_driver/docs/DEVICE_CONFIG.md#stop-measurement-before-rig-startup)).
3. `common::RunSession` (`common/src/session_runner.cpp`) initializes the enabled drivers
   sequentially: AsteRx, FX10, Go-X cameras, LiDARs. The AsteRx warm-up gate holds the rig until
   its readiness condition is met.
4. Each `Run()` executes on its own thread. The coordinator polls all terminate flags and the
   rig-wide guards every 100 ms and propagates the first termination to every driver.
5. After all Run threads join, drivers shut down in reverse order and drain accepted data before
   releasing resources. A failed or partial `Init` is also followed by `Shutdown`.
6. The coordinator finalizes `raw/drivers.json`.

Run results are explicit. Recording or close failures take precedence over an operator stop.

| Exit code | `run.status` in `drivers.json` |
|---|---|
| 0 | `completed`; `interrupted (signal N)` for SIGINT, SIGTERM or SIGHUP |
| 1 | `failed (<driver>)`, `failed (disk)`, `failed (configuration)`, `failed (status persistence)`, `failed (session coordinator)` |

A missing final manifest denotes an unknown result, even if the process has exited. The web GUI's
`AcquisitionController` (`app/services/process.py`) owns process identity, tasks and the stop
latch; UI code only observes its state.

## Build and test

All code runs in the `phenomate-drivers-dev` container (Ubuntu 22.04, GCC 11.4, CMake 3.22; the
repository is mounted at `/workspace`).

```bash
docker exec -w /workspace phenomate-drivers-dev bash -lc \
  'cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)'
docker exec -w /workspace/build phenomate-drivers-dev ctest --output-on-failure -j8
```

Binaries are written to `build/bin/`, test binaries to `build/bin/tests/`. Every build includes
all four drivers, their device tools and the unit tests. `-DPHENOMATE_BUILD_REVISION=<id>` sets
the source revision recorded as `run.git_sha` in `drivers.json` (default `unknown`); configure
does not query Git.

### Targets

| Target | Purpose |
|---|---|
| `PhenomateDrivers` | Unified multi-sensor recorder |
| `ebus_discover`, `ebus_set_ip` | GigE Vision enumeration (`--json` for the GUI); camera re-addressing (FORCEIP + persistent IP) |
| `jai_snapshot` | Go-X single-frame capture; `--stream-s` runs the GUI focus-assist stream |
| `fx10_snapshot` | FX10 preview capture; `--stream-s` runs the GUI focus-assist stream |
| `fx10_reference` | FX10 white/dark reference collection ([workflow](fx10_driver/docs/DEVICE_CONFIG.md#collect-reference-page)) |
| `asterx_lib`, `lms4xxx_lib`, `ebus_camera` | Sensor adapters; Go-X and FX10 share `ebus_camera` |
| `ebus_raw`, `gox_device_config`, `fx10_device_config` | SDK-free RAW recorder; per-camera device configuration |
| `phenomate_common` | SDK-independent infrastructure: session coordinator, logging, strict configuration loading, queues, rig guards, SensorSync host client, `drivers.json` |
| `phenomate_ebus` | eBUS-dependent shared layer (`common/include/ebus`): GenICam environment, error mapping, discovery, IP configuration |

### Dependencies

| Dependency | Source | Used by |
|---|---|---|
| spdlog v1.17.0 (with fmt) | FetchContent, `3rd_party/FetchContent/` | All (single process-wide logger) |
| yaml-cpp 0.9.0 | FetchContent | All configuration loaders |
| nlohmann/json v3.12.0 | FetchContent | RAW metadata and events, device sidecars, `drivers.json` |
| doctest 2.4.11 | Vendored, `3rd_party/doctest/` | All C++ unit tests |
| SensorSync host client | FetchContent of [`qiweishen/sensor_trigger`](https://github.com/qiweishen/sensor_trigger.git), branch `main`, unpinned | Camera trigger session |
| Boost (header-only Asio) | System | lms4xxx TCP |
| Qt5 Core/Network/SerialPort | System | asterx (vendored SsnRx SDK) |
| OpenSSL libcrypto | System | fx10 (SHA-256 of `.pvcfg` files) |
| Pleora eBUS SDK 6.5.1-6797 | `resource/` `.deb`, SHA-256 checked by the Dockerfile; `cmake/FindeBUS.cmake` | Go-X, FX10 |
| Python 3.10 + `requirements.txt` | APT + pip (NiceGUI 3.14.0, NumPy 2.2.6, OpenCV 5.0.0.93, PyYAML 6.0.3) | Web GUI only |

The SensorSync source is populated under `3rd_party/External/sensor_trigger`; only its host-side
`session_client.cpp` is compiled. Upstream keeps the trigger pair-rate rule and the session log
format (format 3) in firmware, so the host carries copies: `common/include/sensor_sync_rates.h`
(mirrored by `app/services/sensor_sync_rates.py`; FX pair 17.882–9847.689 Hz, JAI pair
0.140–9847.689 Hz, 0 = off) and `common/include/sensor_sync_quality.h`. The board remains the
authority: an unachievable rate is answered with `#ERR,bad_freq`.

### Tests

| Suite | ctest entries |
|---|---|
| `common_tests` | One entry |
| `asterx_tests`, `gox_tests`, `fx10_tests`, `lms4xxx_tests` | One entry per test file (doctest `--source-file`) |
| `ebus_raw_tests` | One entry (`ebus.raw`) |
| `tools/check_contracts.py` | `contract_check`, registered when Python is available |

All tests are device-free: protocol frames, parsers, configuration schemas, recorders and on-disk
formats are exercised with synthetic input, largely derived from the vendor manuals' field
tables. The asterx session tests use a fake receiver on localhost; each driver's tests load its
shipped configuration templates. The contract checker verifies the `[Statistics]` `fps=` field
and module/instance routing against the GUI parser (`app/services/driver_stats.py`).

The GUI tests use `unittest`; `app/tests` has no `__init__.py`, so modules are listed explicitly:

```bash
docker exec -w /workspace phenomate-drivers-dev bash -lc \
  'python3 -B -m unittest $(ls app/tests/test_*.py | sed "s|/|.|g; s|\.py$||")'
```

## Running

```bash
sudo setcap cap_sys_nice+ep ./build/bin/PhenomateDrivers   # optional, see below
./build/bin/PhenomateDrivers [path/to/config-main.yaml]    # default: config/config-main.yaml
```

SIGINT, SIGTERM or SIGHUP sets the shared terminate flag; every driver drains and closes in order,
and `All drivers shut down` is logged on completion. The LMS4xxx receive thread requests
`SCHED_FIFO`; without `CAP_SYS_NICE` it runs at normal priority and logs a warning. Compose adds
`SYS_NICE` to the container, but the non-root process also needs the file capability above;
neither `start.bash` nor the GUI applies it.

## Configuration

The executable reads one main configuration, `config/config-main.yaml` by default:

```yaml
General:
    Operator: <name>
    Field: <name>
    Output Directory: ./data
    Enable ASTERX: false             # the GUI's Enable switches write these
    Enable FX10: false
    Enable GOX: true
    Enable LMS4XXX: false
    ASTERX Driver Config Path: ./asterx_driver/config/config-asterx.yaml
    FX10 Driver Config Path: ./fx10_driver/config/config-fx10.yaml
    GOX Driver Config Path: ./gox_driver/config/config-gox.yaml
    LMS4XXX Driver Config Path: ./lms4xxx_driver/config/config-lms4xxx.yaml
Sensor Trigger:
    Port: "/dev/serial/by-id/usb-Teensyduino_USB_Serial_16838390-if00"   # "" = no board
Guards:
    Disk Min Free GiB: 5             # stop the run below this (0 = off)
    Disk Warn Free GiB: 20
    No Data Warn S: 15
    No Data Abort S: 60              # stop the run after this much silence (0 = off)
```

All configuration files follow one schema policy (`common/include/config_util.h`): unknown keys
are errors that name the key and list the accepted set; omitted keys take the code defaults;
present but malformed values are errors, never a request for a default; errors carry the dotted
key path; floating-point values must be finite and within range; negative values are refused for
unsigned fields. In the main configuration, `General` is required, `Guards` and `Sensor Trigger`
are optional, an omitted Enable switch means `false`, and paths must be non-empty. The LMS
configuration is always parsed, because step 2 of the session uses it.

Each driver configuration is a commented YAML template next to its driver; the comments cite the
manual pages behind non-obvious values, so the GUI edits these files as text. The shipped
templates enable LMS4xxx NTP and SensorSync with `trigger.mode: external` in both camera
configurations; Go-X PTP is disabled. The SensorSync serial port is set only in the main
configuration; camera configurations declare their channel and pulse rate.

Configuration changes apply at the next recording start. The GUI checks path and modification
time before saving, blocks writes while a recording starts or stops, while a device tool runs or
while process ownership is uncertain, and keeps a `.bak` copy. `PHENOMATE_CONFIG_ROOT` and
`PHENOMATE_DATA_ROOT` optionally confine resolved configuration and output paths.

## Session output

```text
<Output Directory>/<YYYYMMDD_HHMMSS>/
└── raw/
    ├── log_<ts>.log            # session log (trace level)
    ├── drivers.json            # run manifest: identity, lifecycle, versions, final results
    ├── config/                 # config-<name>_<ts>.yaml copies of the configurations used
    ├── sensor_trigger.log      # SensorSync events: triggers (T), exposures (S), PPS (P), NMEA (Z)
    ├── asterx/                 # asterx-<UTC>-N.sbf, live_*.csv
    ├── gox/<camera id>/        # ebus-raw segments, device.json, events.jsonl
    ├── fx10/fx10_<UTC>/        # ebus-raw segments, device.json, events.jsonl
    └── lms4xxx/                # scan_<id>_<ts>_NNN.bin, .csv, .json
```

| Format | Content | Specification |
|---|---|---|
| SBF | AsteRx blocks exactly as delivered by the SDK (CRC-checked, never re-serialized), from configuration, warm-up and recording; readable by RxTools and `sbf2rin` | [AsteRx](asterx_driver/docs/DEVICE_CONFIG.md) |
| `ebus-raw-eBUS_SDK_v<SDK version>` | Unmodified SDK buffer bytes with per-record JSON layout, status and timestamps, and CRC-32C over prefix, metadata and payload; `device.json` (`ebus-device-eBUS_SDK_v<SDK version>`) stores configuration and readbacks, `events.jsonl` health and completion | [`ebus_driver/README.md`](ebus_driver/README.md) |
| `lms4xxx-cola-b` v1 | Complete received telegrams (STX, length, command, checksum) in BIN files; CSV index with byte offsets, host reception and clock observations; JSON metadata with per-split completion | [LMS4xxx](lms4xxx_driver/docs/DEVICE_CONFIG.md#recording-format) |

The `ebus-raw` suffix is the compiled SDK header version (`PvVersion.h` `NVERSION_STRING`, also
`sdk.header_version`), not the runtime SDK or the project version; the RAW header carries its own
layout version (`1`). Each camera has a receive worker that copies SDK bytes to owned memory
before requeueing the buffer, and a separate writer; queue exhaustion fails the run and records
the rejected buffer. `fdatasync` does not make the LMS BIN/CSV/JSON triple atomic; check split
metadata, file lengths and the session verdict together.

GUI runtime files are kept apart from recordings, under `PHENOMATE_RUNTIME_DIR`
(default `app/_runtime/`): `acquisition.json` (status of the current run), `tool-operation.json`
(device tool journal), `controller.lock`, and the work directories `snapshot/`, `snapshot_fx10/`,
`focus/` and `focus_fx10/`.

## Health guards

Two guards apply to the whole rig and are configured in `Guards:`
(`common/src/session_runner.cpp`):

| Guard | Measurement | Shipped values (code default) |
|---|---|---|
| Disk floor | `statvfs` `f_bavail` of the session filesystem, before bring-up and once per second | Warn < 20 GiB, stop < 5 GiB (same) |
| No-data watchdog | `IDriverApp::MicrosSinceLastData()` per driver | Warn after 15 s (5 s), stop after 60 s (same) |

A driver that is legitimately idle (external trigger without pulses, receiver warm-up) reports
no value and is not watched while that state lasts. A watchdog trip is logged as
`<Driver> driver stopped: no data (...)` and becomes the run's failure reason. Device-specific
guards are documented with each driver:

| Driver | Device-specific guards |
|---|---|
| AsteRx | Failures before the first complete configuration are fatal without retry; warm-up gate (SBF duration, optional FINETIME); 3 s SBF silence timer that reconnects during warm-up (60 s total recovery deadline) and is fatal afterwards; damage, link gaps and receiver resets mark the recording incomplete and are fatal after readiness; write failures and queue overflow are always fatal |
| Go-X | PTP slave-status admission and runtime guard (when PTP is enabled); thermal warning; `Counter0` missed-trigger accounting; SensorSync log integrity and stall; the first dropped, lost or incomplete frame is fatal |
| FX10 | Unusable-stream abort; thermal limits; recorder-failure classification; SensorSync log integrity and stall; the first lost or unrecorded frame or missed trigger is fatal |
| LMS4xxx | Full first-scan structure and scale check, then timestamp monitoring; NTP server and device health, clock-step check; framing, transport and writer failures are fatal; counter gaps affect the final verdict |

The snapshot and reference tools run outside `main`, so `Guards:` does not apply to them.

## Logging

Every line has the form `[HH:MM:SS] [level] [Module]: message`. The session log file is the
complete record. The stderr copy is best-effort: on a pipe (GUI, `docker exec`) the console sink
is non-blocking and drops lines rather than stall a driver thread, and reports the number dropped
at the end.

| Rule | Convention |
|---|---|
| Module tags | The coordinator logs as `MainApp`, the AsteRx and LMS4xxx app layers as `AsteRxApp` and `LMS4xxxApp`, the shared camera runtime as `GoX` or `FX10`; other layers use short tags (`AsteRx`, `LMS4xxx`, `eBUS`) |
| Prefixes | Instance tag first (camera or LiDAR id), then subsystem: `[Writer]`, `[eBUS]`, `[TCP]`, `[Live]`, `[TriggerLog]` |
| Statistics | Periodic lines start with `[Statistics]`, use double-space-separated `key=value` fields and report `rate=N.N Hz` (sensor output) and `fps=N.N` (frames written); new fields are appended; the shutdown line is `[Statistics] [inst] Final: ...` |
| Errors | Lower layers return native error codes or exception types; device adapters latch failures; severity never determines lifecycle state |

```text
[FX10]:       [Statistics] [fx10] up=60s rate=50.0 Hz  fps=49.8  frames=1200 buffers=1200 lost=0 queue=0/20
[GoX]:        [Statistics] [cam0] up=65s rate=5.0 Hz  fps=5.0  frames=325 buffers=325 lost=0 queue=0/11
[LMS4xxxApp]: [Statistics] [Front_Center_Laser] up=00:00:10  rate=600.0 Hz  fps=598.0  ntp=SYNCHRONIZED  frames=6000  ...
[AsteRx]:     [Statistics] blocks=48210  bytes=12.4 MB  files=1  crc_fail=0  length_errors=0  discarded_bytes=0  ...
```

`fps=` is a GUI contract parsed by `app/services/driver_stats.py` and checked by
`tools/check_contracts.py`. AsteRx records a byte stream and has no `fps=`; during warm-up its line
carries `warmup=<recorded>/<min>  finetime=0|1`. LMS4xxx `ntp=` tokens are listed in
[its documentation](lms4xxx_driver/docs/DEVICE_CONFIG.md#time).

## Web GUI

`app/` is a NiceGUI control panel on port 8619, running in the same container as the C++ binaries.
Its local control service owns acquisition, device tools and configuration writes. Module layout,
data-protection rules and the camera workflows are described in [`app/README.md`](app/README.md).

| Page | Function |
|---|---|
| `/` Overview | Start and stop recording; Operator, Field and Output Directory; per-driver Enable switches; sensor health, storage, written-frame rates and final run results |
| `/config` | Text editor for the main and driver configurations with YAML validation, conflict detection, atomic save and `.bak` |
| `/logs` | Session log with level and module filters |
| `/live` | On-demand Go-X and FX10 previews from the running session |
| `/asterx` | Live AsteRx telemetry from the session's CSV sidecars and route display |
| `/sessions` | Recording history: session list from each `raw/drivers.json` with date, device and state filters (20 per page) and a map of the selected session's track |
| `/reference` | FX10 white/dark references and Go-X ColorChecker calibration |
| `/camera` | Camera Tools: shared discovery, Set IP, Go-X and FX10 exposure workbenches and focus assist |

Rules that protect recordings:

- The GigE control channel is exclusive. Discovery is allowed during a verified recording; a camera
  owned by the recording is marked in use, and recording cannot start while a camera tool runs.
- Previews never write configuration. Workbench values reach the snapshot tools as command-line
  overrides; only **Apply to config** writes them, to both the recording and the snapshot
  configuration, as text edits with readback.
- Set IP is the only operation that writes camera network settings. `ebus_set_ip` forces the IP by
  MAC and then writes the persistent IP; on request the GUI then updates `device.ip` in the
  driver configuration (`app/services/config_store.py`).
- The output directory is chosen with an in-page browser of server paths within
  `PHENOMATE_DATA_ROOT` (Compose: `/workspace`; the shared disk is `/workspace/dataset`) and is
  saved explicitly; it applies to the next recording.

### Lifecycle protocol and recovery

The acquisition process publishes `status_schema: phenomate-run-v1` in `raw/drivers.json`,
including process identity (`pid`, `start_ticks`, `boot_id`), session location and lifecycle
phase (`initializing → running → stopping → finished`). The controller passes
`PHENOMATE_STATUS_FILE` so the same document also appears at `<runtime>/acquisition.json`. GUI
readiness and configuration locks follow this protocol only; logs supply diagnostics and `fps=`.
The Recording history page also accepts manifests written before the rename (`amiga-run-v1`).

Recovery matches process and session identity rather than the newest folder. A successful result
requires an ended manifest, `recording_failed: false` and a completed or interrupted status, and
an attached process must exit cleanly. Acquisition receives only SIGTERM, so it can drain. Tools
journal their identity before they are released to run; a forced stop applies only to tools and
is verified. When ownership or exit cannot be confirmed, conflicting controls stay locked. Closing
the browser does not stop recording; stopping the container requests a stop and waits for
cleanup. All control services must share one `PHENOMATE_CONTROL_LOCK`.

### Settings

| Variable | Default and purpose |
|---|---|
| `PHENOMATE_GUI_HOST`, `PHENOMATE_GUI_PORT` | `0.0.0.0`, `8619`: listening address and port |
| `PHENOMATE_MAIN_CONFIG` | `<repo>/config/config-main.yaml` |
| `PHENOMATE_SNAPSHOT_CONFIG` | `<repo>/gox_driver/config/config-gox-snapshot.yaml`: Go-X snapshot and ColorChecker profile |
| `PHENOMATE_FX10_SNAPSHOT_CONFIG` | `<repo>/fx10_driver/config/config-fx10-snapshot.yaml`: FX10 snapshot and reference profile |
| `PHENOMATE_CONFIG_ROOT`, `PHENOMATE_DATA_ROOT` | Unset: optional path boundaries; Compose sets both to `/workspace` |
| `PHENOMATE_RUNTIME_DIR` | `<repo>/app/_runtime`: journals and temporary previews |
| `PHENOMATE_CONTROL_LOCK` | `<runtime>/controller.lock`: exclusive control lock |

Variables from before the rename (`AMIGA_*`) are not read. The retired `AMIGA_GUI_MODE` and
`AMIGA_CONTROLLER_*` settings have no successor.

## Docker deployment

`.devcontainer/docker-compose.yml` defines one service, `phenomate-drivers-dev`, built from
`.devcontainer/Dockerfile` (Ubuntu 22.04, target `drivers`). The image contains the C++ toolchain,
the eBUS SDK under `/opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64`, and the GUI's Python packages
(`/opt/phenomate-python`, outside the bind mount). It does not compile the project. The container
runs `python3 -B -m app.main` with host networking, mounts the repository at `/workspace`, the
shared disk `/mnt/SharedData/Post_Processing_Data` at `/workspace/dataset` and `/dev`, and grants
`NET_ADMIN`, `NET_RAW` and `SYS_NICE`. UID/GID default to `1000:1000` (`PHENOMATE_UID`,
`PHENOMATE_GID`). The health check queries `/healthz` through `.devcontainer/gui_healthcheck.py`;
it does not test sensors. Compose allows 180 s for shutdown before forcing termination.

Host prerequisites: `net.core.rmem_max` must cover the largest configured camera
`socket_rx_buffer_mb` (shipped configurations: 32 MiB = 33554432 bytes); the eBUS SDK caps larger
requests, and the driver logs the request and the actual readback. The Pleora KEYLOK dongle needs
the udev rule `resource/99-pleora-keylok.rules`. On the edge computer, `start.bash` installs that
rule, sets `rmem_max` (until reboot), recreates the container and builds the project; it is not
intended for development machines, which use the Compose commands below.

```bash
docker compose -f .devcontainer/docker-compose.yml up -d --build
docker compose -f .devcontainer/docker-compose.yml logs --tail=100 -f phenomate-drivers-dev
docker compose -f .devcontainer/docker-compose.yml restart phenomate-drivers-dev
```

Stop any active recording before rebuilding, recreating or stopping the container. Do not add
`--remove-orphans`: the Compose file sets no project name, so the project defaults to the
directory name `devcontainer`, which other repositories' devcontainers share, and their
containers would be removed.

X11 programs such as eBUS Player run in the container via `.devcontainer/x11-exec.sh`, which
passes the caller's `DISPLAY` (for example an `ssh -Y` session) and X cookie:

```bash
.devcontainer/x11-exec.sh bash -c '"$EBUS_SDK_ROOT"/bin/eBUSPlayer'
```

To restrict the GUI to the Tailscale interface, set `PHENOMATE_GUI_HOST="$(tailscale ip -4)"` and
recreate the service (`up -d --no-build --force-recreate phenomate-drivers-dev`); for persistence,
put the variable in `.devcontainer/.env` and pass `--env-file`. The GUI has no login; choose the
listening scope accordingly.

## Repository layout

```text
phenomate_drivers/
├── main.cpp            # process entry: session setup, configuration copies, driver composition
├── CMakeLists.txt      # unified build and tests
├── start.bash          # edge-computer startup: udev, rmem_max, container recreation, build
├── .devcontainer/      # Dockerfile, Compose service, IDE settings, X11 helper
├── cmake/              # Dependencies.cmake, FindeBUS.cmake
├── docs/               # architecture documents (drivers, GUI)
├── 3rd_party/          # FetchContent sources, SensorSync source, vendored doctest
├── config/             # config-main.yaml
├── resource/           # eBUS SDK .deb, KEYLOK udev rule, devices_ip.yaml
├── common/             # phenomate_common, phenomate_ebus, tests
├── asterx_driver/      # AsteRx session, SBF recording, live CSV, tests, docs
├── ebus_driver/        # shared camera runtime and RAW recorder, tests
├── gox_driver/         # Go-X configuration, control, PTP, tools, tests, docs
├── fx10_driver/        # FX10 configuration, control, tools, tests, docs
├── lms4xxx_driver/     # CoLa B communication, verification, RAW recorder, tests, docs
├── app/                # web GUI: decoder/, services/, ui/, tests/
├── tools/              # check_contracts.py
├── requirements.txt    # GUI Python dependencies
└── AGENTS.md           # review protocol for automated reviewers
```

## Documentation

| Document | Content |
|---|---|
| [`docs/architecture-drivers.md`](docs/architecture-drivers.md) | C++ driver architecture: C4 context, containers and components, CMake targets, runtime sequences, threads and queues, state machines, failure handling, deployment |
| [`docs/architecture-gui.md`](docs/architecture-gui.md) | Web GUI architecture: C4 views, page/service/decoder layering, lifecycle and tool sequences, state machines, concurrency, C++ contracts, deployment |
| [`asterx_driver/docs/DEVICE_CONFIG.md`](asterx_driver/docs/DEVICE_CONFIG.md) | Login and baseline, configuration sequence, warm-up gate, SBF recording, failure policy |
| [`gox_driver/docs/DEVICE_CONFIG.md`](gox_driver/docs/DEVICE_CONFIG.md) | Factory user set, apply plan and readback, pixel format, PTP guards, triggering, dataset metadata |
| [`fx10_driver/docs/DEVICE_CONFIG.md`](fx10_driver/docs/DEVICE_CONFIG.md) | Factory preparation and `.pvcfg` restore, write order, exposed parameters, triggering, reference collection |
| [`lms4xxx_driver/docs/DEVICE_CONFIG.md`](lms4xxx_driver/docs/DEVICE_CONFIG.md) | `mSCloadappdef` baseline, telegram sequence, time, pre-start stop, recording format |
| [`lms4xxx_driver/docs/sopas_filter_polarity.md`](lms4xxx_driver/docs/sopas_filter_polarity.md) | Filter status-byte polarity established from a SOPAS capture |
| [`ebus_driver/README.md`](ebus_driver/README.md) | Shared camera runtime and `ebus-raw` format |
| [`app/README.md`](app/README.md) | GUI structure, data-protection rules, camera tools, focus assist, reference capture |
| [`resource/devices_ip.yaml`](resource/devices_ip.yaml) | Rig addresses and host interfaces per device |
| `3rd_party/External/sensor_trigger/README.md` | SensorSync protocol and log format (available after configure) |

The device documents record what each driver writes, what it deliberately does not write, and
the manual page behind each decision; read them before changing code that talks to hardware.

## Limitations

- Hardware behavior, sustained throughput, real synchronization accuracy and the maximum drain
  time at shutdown have not been measured.
- The documented checks make observed loss explicit; they do not prove that hardware, SDK or
  storage cannot lose data.
- Display paths (browser, compositor, monitor bit depth) are not verified.
- Remote access over Tailscale and the X11 helper have not been tested from this repository.
