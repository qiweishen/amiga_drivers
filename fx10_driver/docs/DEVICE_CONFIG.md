# FX10 device configuration

## Scope

This document describes how the driver configures the Specim FX10e (GigE Vision, Pleora eBUS SDK
6.5.1) on every bring-up, which settings it leaves to Specim's configuration file, and how it reacts
to failures. The same sequence runs in `PhenomateDrivers`, `fx10_snapshot` and each phase of
`fx10_reference`; nothing is saved on the camera. The code lives in `fx10_driver/` and in
`ebus_driver/src/fx10_config.cpp`, the FX10 hooks of the shared `ebus_driver/src/camera_driver.cpp`.
Page references `(p. N)` refer to the Specim FX10 User Manual 2.2
(`fx10_driver/docs/161584-Specim-FX10-Reference-Manual.pdf`), which uses Lumo/ASCII feature names
and defers GenICam to the GigE Vision standard (p. 20); node names here come from the code or the
Specim `.pvcfg` file.

## Network prerequisites

| Item | Requirement |
|---|---|
| Address | `10.95.0.100` on host interface `enp1s0f0` (`10.95.0.1/24`), per `resource/devices_ip.yaml`. The driver connects by `device.mac` (discovery; wins over `ip`) or `device.ip` and never re-addresses the camera. |
| MTU | 9000 on `enp1s0f0` (`resource/01-network-manager-all.yaml`); the manual asks for the maximum jumbo-frame value (p. 17). |
| Packet size | `network.packet_size: 0` calls `PvDeviceGEV::NegotiatePacketSize`; on failure the driver logs a WARN and sets 1476 bytes. A nonzero value is set directly. Either overrides the `.pvcfg` value (`GevSCPSPacketSize = 8228`); the final value is in `device.json` → `runtime.packet_size`. |
| Socket buffer | `network.socket_rx_buffer_mb: 32` requests 33 554 432 bytes through `SetUserModeSocketRxBufferSize`; host `net.core.rmem_max` must be at least that value. When it is lower, eBUS 6.5.1 caps the request and returns `INVALID_PARAMETER`; the driver logs a WARN, continues and records the request, an `rmem_max` snapshot and the effective size read after `PvStreamGEV::Open`. Otherwise an effective size below the request, a zero readback or any other SDK error fails bring-up. The driver never changes host settings; see "Prerequisites" in the top-level `README.md`. |

## Bring-up sequence

| Step | Stage | Action |
|---|---|---|
| 1 | Init | Connect with `PvDeviceGEV::Connect`. |
| 2 | Init | [Factory preparation](#factory-preparation): `CameraHeadFactoryReset`, then `UserSetLoad`. |
| 3 | Init | [Restore the Specim `.pvcfg`](#specim-pvcfg-restore). |
| 4 | Init | Open the stream: socket buffer, packet size, stream destination. |
| 5 | Init | Walk the [write plan](#write-plan); check final image settings and the sensor window. |
| 6 | Init | Read `Width`, `Height`, `OffsetX`, `OffsetY`, `PayloadSize`, `PixelFormat`, `EnStatusLine`; resolve wavelengths; read the `Counter1_EventSource` binding. |
| 7 | Init | Read `GevGVSPExtendedIDMode`; allocate buffers; write `device.json`; register with SensorSync. |
| 8 | Run | [Open the shutter](#mechanical-shutter) (close it for a dark reference); read the missed-trigger baseline; log `profile_start`. |
| 9 | Run | Require an unchanged `PayloadSize`; `StreamEnable`; `AcquisitionStart`; arm SensorSync. |

`PhenomateDrivers` runs steps 1–7 for each enabled driver in turn, then steps 8–9 for all drivers
concurrently. Any failure fails the FX10 driver; in `PhenomateDrivers` it stops the rig and the
process exits with status 1. Tool exit codes are listed under [Tools](#tools).

### Factory preparation

The manual documents neither command; the driver requires both nodes and executes each exactly once.

| Phase | Behavior |
|---|---|
| Before Execute | Best-effort `AcquisitionStop` (result only logged); user sets load only while acquisition is stopped. |
| During Execute | `AnswerTimeout` raised to at least 10 000 ms and `DisconnectOnAnyTimeout` cleared when available (eBUS Player precedent); both are restored afterward. |
| `UserSetLoad` | `UserSetSelector` is not written but logged, so the camera loads whichever set is selected. |
| Result | A non-OK Execute is fatal and not replayed. No `IsDone` poll is issued: eBUS 6.5.1 `UserSetsManager::Load` does not poll it either, and on this camera one `IsDone` call took about 10 s before a link-loss notification. |
| Readiness | Node cache invalidated; `Width`, `Height`, `PayloadSize` > 0 and a nonempty `PixelFormat` required. Polled every 200 ms for up to 15 s on the same connection, without reconnecting or re-executing; link loss or timeout is fatal. Blocking SDK calls can overrun the budget. |
| Evidence | `device.json` → `factory_preparation` lists both commands. Acknowledgement and readiness do not prove every factory value; the driver verifies only the values it writes. |

### Specim .pvcfg restore

| Step | Rule |
|---|---|
| Path | `device.ebus_config`; both shipped YAML files use `fx10_driver/resource/ebus_config/077000060381/3210798.pvcfg`, saved by Specim for this camera. A relative path resolves two directories above the executable (the project root for `build/bin/`). The loader requires a `.pvcfg` file inside a named directory. `""` skips the restore and the sensor-window guard. |
| Serial | The trimmed `DeviceSerialNumber` must equal the directory name. |
| Checksum | `checksums.txt` beside the file (`sha256sum` format) must hold exactly one entry for it, equal to the file's SHA-256 (OpenSSL). |
| Geometry | `Width`, `Height`, `OffsetX`, `OffsetY`, `BinningHorizontal` and `BinningVertical` are parsed from the single inner `<device>` section, each exactly once as a decimal integer. Shipped: 1024, 224, 112, 285, 1, 2 (detector window 1024 × 448). |
| Apply | `PvConfigurationReader::Load` (exactly one device), then `Restore(0, device)` twice, whole file, unfiltered; control readiness is checked once after each pass. The `<stream>` section is not applied. Each pass also writes `MotorShutter_PulseFwd = 204`, then `MotorShutter_PulseRev = 200`. |
| Verify | A fresh readback of the six geometry nodes must equal the file values. |
| Failure | A failed check before loading restores nothing. A non-OK `Restore` result or a geometry mismatch is fatal and not retried; per-parameter SDK errors are logged as WARN. |
| Evidence | `device.json` → `ebus_config`: `status`, `path`, `device_serial`, `sha256`, `file_geometry`, each pass's `restore_result` and `restore_errors`, `readback_geometry`. |

Two passes are needed because the file lists `Height` before `BinningVertical`: a pass starting
from another binning writes `Height = 224` under that binning, and the following binning write
rescales it (suspected cause of 112 instead of 224 bands at 1 × 2). The second pass starts from the
file's binning.

### Write plan

`BuildApplyPlan()` is pure and SDK-free; `fx10_driver/tests/test_apply_plan.cpp` pins its order. The
node names in `fx10::node` are marked in the code as verified with eBUS Player 6.5.1.

| Order | Node | Value | Note |
|---|---|---|---|
| 1 | `BinningHorizontal` | `spatial_binning` | Binning first: it changes the geometry later values depend on (p. 46). |
| 2 | `BinningVertical` | `spectral_binning` | |
| 3 | `PixelFormat` | `pixel_format` | |
| 4 | `ExposureMode` | `Timed`; `TriggerControlled` for external trigger with `pulse_width` | Before `ExposureTime` (p. 29). |
| 5 | `ExposureTime` | `exposure_ms` × 1000 µs | Omitted under `TriggerControlled`. |
| 6 | `AcquisitionMode` | `Continuous` | The only supported mode. |
| 7 | `TriggerSelector` | `trigger.selector_entry` | Both trigger modes. |
| 8 (freerun) | `TriggerMode`, `EnAcquisitionFrameRate`, `AcquisitionFrameRate` | `Off`, `true`, `frame_rate_hz` | Constant frame rate (p. 28). |
| 8 (external) | `TriggerSource`, `TriggerActivation`, `TriggerDelay`, `TriggerMode`, `EnAcquisitionFrameRate` | `source_entry`, `RisingEdge` or `FallingEdge`, `delay_ms` × 1000 µs, `On`, `false` | `EnAcquisitionFrameRate` is written in both modes so an earlier session's value never persists (p. 28). |
| 9 (external) | `Counter1_EventSource`, `Counter1_Reset` | `MissedTrigger`, execute | Missed-trigger counter (p. 27). |
| 10 | `features.raw` entries | as configured | Last, in file order. |

| Write | Readback rule |
|---|---|
| Every write | Read back immediately and recorded in `device.json` → `applied` (`requested`, `attempted`, `readback`, `status`). |
| Driver-derived integer or float | Any change by the camera fails bring-up (float tolerance 0.1 % relative, at least 1e-9). |
| `AcquisitionFrameRate` | Clamped to the node's live `[min, max]` before writing, because the FX10e rejects out-of-range writes instead of clamping; WARN, and the clamped value is recorded as `attempted`. |
| Any enum, boolean or string | A mismatch fails bring-up, including in `features.raw`. |
| `features.raw` integer or float | A clamp logs a WARN; an unknown node fails bring-up. The loader rejects `AcquisitionStart`, `AcquisitionStop` and both `MotorShutter_*` nodes. |
| `Counter1_*` | Optional: an absent node or failed write logs a WARN and is skipped. |
| `LineSource` | When the camera exposes `LineSelector`, an applied `LineSelector` write must precede it. |

After the plan, `PixelFormat` and both binnings must equal the configuration. After a `.pvcfg`
restore, `OffsetX`, `OffsetY`, `Width × BinningHorizontal` and `Height × BinningVertical` must also
equal the restored values: binning may change, the calibrated window may not (p. 25). At 1 × 1 the
readback must be 1024 × 448, at 1 × 4 it must be 1024 × 112. `device.json` → `sensor_window` keeps
both readbacks and the status `unchanged`, `changed` or `not-checked`.

## YAML parameters

Both files have a strict schema: unknown keys are errors, omitted keys keep the code defaults.
`config/config-fx10.yaml` drives acquisition; `config/config-fx10-snapshot.yaml` drives the Camera
workbench and reference capture. Factory values are from Table 8 (p. 44).

| Key | Default | `config-fx10.yaml` | Constraint |
|---|---|---|---|
| `acquisition.spatial_binning` | 1 | 1 | 1, 2, 4, 8 → 1024, 512, 256, 128 samples. Factory 1. |
| `acquisition.spectral_binning` | 2 | 2 | 1, 2, 4, 8 → 448, 224, 112, 56 bands in the shipped window. Factory 2; the manual lists only 2, 4 and 8 (p. 46). |
| `acquisition.pixel_format` | `Mono12Packed` | `Mono12Packed` | `Mono12Packed`, `Mono12`, `Mono10Packed`, `Mono10`, `Mono8`. Factory `Mono12`. |
| `acquisition.exposure_ms` | 5 | 10 | > 0 and ≤ 419 ms; ≤ 90 % of the frame period, `0.9 × 1000 / frame_rate_hz` (inclusive), in every trigger mode. Factory 5 ms. |
| `acquisition.frame_rate_hz` | 50 | 51 | > 0; ≤ 163.02 Hz at 1 × 1 binning, ≤ 324.373 Hz otherwise. Freerun: the written rate. External: the expected pulse rate, which sizes buffers and, with SensorSync, is the pulse rate. Factory 50. |
| `acquisition.trigger.mode` | `external` | `external` | `freerun` or `external`. |
| `acquisition.trigger.activation` | `rising` | `rising` | `rising` or `falling`. |
| `acquisition.trigger.delay_ms` | 0 | 0 | 0–419 ms. |
| `acquisition.trigger.exposure_control` | `camera` | `camera` | `camera` or `pulse_width` (external trigger only). |
| `acquisition.trigger.selector_entry`, `source_entry` | `FrameStart`, `Line0` | same | Written verbatim; the YAML comment maps `Line0` to ISO_TRIGGER pin 8. |
| `sensor_trigger.enabled`, `trigger_channel` | `false`, 0 | `true`, 0 | Channel 0–3. With SensorSync and external trigger, `frame_rate_hz` must lie within [17.882, 9847.689] Hz for channels 0/1 (FX pair). |
| `reference.duration_s` | 5 | 5 | 0.1–3600 s per reference phase. |
| `output.rotation.max_lines` | 0 | 0 | SDK records per RAW segment, including flagged buffers; 0 disables. |
| `output.rotation.max_mb` | 2048 | 2048 | Segment size limit in MiB, including record headers and metadata; 0 disables. |
| `output.flush_interval_mb` | 64 | 64 | 0–4096; `fdatasync` on the writer thread after that many bytes; 0 syncs only at segment boundaries. |
| `output.on_gap` | `record` | `pad_zero` | Accepted and ignored; RAW never inserts zeros. |
| `output.wavelengths.source` | `none` | `none` | `none` records DN without a wavelength axis. `file`/`list` need a `calibration` block whose serial, `samples`, `bands`, `offset_x` and `offset_y` match the delivered image, with the status line off. `grid` parses but fails bring-up. |
| `output.max_duration_s`, `max_frames` | 0, 0 | 0, 0 | Seconds from immediately before `AcquisitionStart`; admitted valid image buffers. 0 = unlimited. |

The frame-rate ceilings and the 419 ms exposure maximum are operator-set values (2026-11-08).
`CheckAcquisitionTiming` runs in the loader and again in `fx10_snapshot` after CLI overrides. The
recorded band count is the `Height` readback. The remaining `network.*` and `logging.*` keys are
documented inline in the YAML files.

## Settings the driver does not write

The write plan does not touch the following. Where a value is listed, the restored `.pvcfg` sets
it; without a `.pvcfg` it keeps whatever factory preparation left. `device.json` → `final_readback`
records `AberCorrection_Enable`, `EnStatusLine`, `MROI_Enable`, `DigitalGain`, `DigitalOffset`,
`BlackLevelOffset`, `Correction_Mode`, `TestPattern`, `LinLog_Mode`, `BinningBitshift` and
`BinningSaturationMode`.

| Feature | `.pvcfg` value | Reason the driver does not write it |
|---|---|---|
| AIE (`AberCorrection_Enable`) | 1 | In-camera wavelength calibration with smile and keystone correction that interpolates every pixel at a fractional address (p. 32–33), so recorded pixels are resampled. The calibration pack with "FX" in its name applies with AIE on and is recommended (p. 44). |
| Status line (`EnStatusLine`) | 0 | When enabled it replaces the last image row with camera status (p. 34); recorded as `profile.status_line`, and wavelength assignment is refused when it is on. |
| MROI (`MROI_Enable`) | 0 | Factory disabled (p. 44); the image area is the calibration ROI. |
| Digital gain (`DigitalGain`) | `Gain1x` | ×2, ×4 and ×8 are binary shifts that zero the LSBs and produce missing codes (p. 34). |
| Fine gain, digital offset | 1, 0 | Part of the factory calibration; the digital offset is unavailable at ×1 (p. 34). |
| NUC (`Correction_Mode`) | `OffsetHotpixel` | Factory correction; with AIE on, hot-pixel correction runs regardless (p. 34). |
| Readout (interleave) mode | — (factory: enabled, p. 44) | Switching it requires a new `BlackLevelOffset` (103 in the file) and new image-correction settings (p. 22). |
| ROI (`OffsetX`, `OffsetY`, `Width`, `Height`) | 112, 285, 1024, 224 | "ROI feature is reserved for this purpose alone. Do not change these values." (p. 25) |
| Test image (`TestPattern`) | `Off` | Test images are generated in the FPGA independently of the sensor (p. 36). |
| Burst and software trigger | — | Exposures are paced by freerun or by external pulses (p. 30–31). |
| Strobe delay and duration | — | The strobe output is the exposure signal that SensorSync logs. |
| `UserSetSave`, `UserSetSelector` | — | Configuration is volatile; every bring-up starts from factory preparation. |
| `DeviceReset` | — | It reboots the camera and may re-address it; the manual documents only a power cycle (p. 43). |
| Persistent IP nodes, `PvDeviceGEV::SetIPConfiguration` | — | Re-addressing is an operator action through `ebus_set_ip`; a camera on a foreign subnet fails `PvDeviceGEV::Connect`. |

## Mechanical shutter

The manual does not document the shutter nodes; the definition is operator-supplied, and 200 and
204 are the Specim `.pvcfg` values. Table 8 states that the shutter opens at power-up (p. 44); the
driver does not rely on that state and has no YAML switch for the shutter.

| Node | Range | Sequence | Effect | Used by |
|---|---|---|---|---|
| `MotorShutter_PulseRev` | 1–255 | 1, then 200 | Opens | Acquisition, snapshots, white reference phase |
| `MotorShutter_PulseFwd` | 1–255 | 1, then 204 | Closes | Dark reference phase |

The operation runs after the write plan and before `StreamEnable`. It requires an available,
writable `PvGenInteger` whose reported range contains both values. For each value it calls
`SetValue`, invalidates the node cache, reads the value back with `GetValue` and requires equality
before the next write; it always writes, never calls `PvGenCommand::Execute` and never retries. A
missing or wrongly typed node, an unsupported range, an SDK error, a mismatch, cancellation or link
loss stops startup. A matching readback confirms the stored value, not motion; no settling time is
applied. `device.json` → `profile.shutter_request` holds the intent (`open` or
`closed-dark-reference`); `events.jsonl` → `profile_start.actions.shutter_preparation` holds
`status` (`open-pulses-read-back`, `close-pulses-read-back` or `failed`), `sequence`,
`writes_acknowledged`, `readbacks`, `node_min`, `node_max` and `state_verified: false`.

## Triggering and timing

| `trigger.mode` | `sensor_trigger.enabled` | Behavior |
|---|---|---|
| `external` | `true` | Registers on the rig's shared `common::SensorSyncHub` with `pulse_hz = frame_rate_hz`; pulses start once every registered camera has armed. Frames are required. |
| `external` | `false` | Not registered; the camera waits for pulses on `source_entry`. A run without frames is not a failure. |
| `freerun` | `true` | Registered at 0 Hz (strobe logging only). |
| `freerun` | `false` | No SensorSync. |

The board's port is set in `config/config-main.yaml` ("Sensor Trigger: Port"); the timing log is
`raw/sensor_trigger.log` in the session, and channels 0/1 share one rate. The camera registers
during Init, arms after `AcquisitionStart` and disarms before `AcquisitionStop`. A hub error, a
timing log stalled for more than 15 s or participants not all armed within 30 s fails the session;
the end of the shared session stops the camera. At shutdown with pulses, exposure and frame
accounting from the log must be complete; in freerun only the log's integrity is checked. The
shipped `features.raw` writes `LineSelector = LineOut0` and `LineSource = ExposureActive` to drive
the strobe output that SensorSync logs.

Each record keeps the unscaled `PvBuffer::GetTimestamp` (unit and epoch unverified) and host
realtime and monotonic times sampled after `RetrieveBuffer`, which mark reception, not exposure;
`device.json` → `timing.association_verified` is `false`.

| Health value | `device_health` events at start, every 60 s and at stop |
|---|---|
| Missed triggers | `Counter1_Value` against a baseline read before `AcquisitionStart`. The binding is verified when `Counter1_EventSource` reads `MissedTrigger`; with a verified binding, a positive delta or a counter regression fails the session. |
| Temperatures | `DeviceTemperatureSelector` is written before each read of `ProcPCB` and `FPGA` (cancel limits 80 °C and 90 °C, p. 44). A WARN is logged at ≥ 75 °C or ≥ 85 °C and re-armed at ≤ 70 °C and ≤ 80 °C; unreadable values are `null`. |

## Recording

The shared engine copies the complete `GetAcquiredSize()` bytes of each SDK buffer before requeue;
it does not unpack pixels, strip padding or chunk data, remove the status line or build an ENVI
cube. One SDK image is one pushbroom line: width holds spatial samples, height holds spectral rows.
The RAW layout is specified in `ebus_driver/README.md`; offline conversion belongs to
`PhenomateDecoders`. `PhenomateDrivers` copies the YAML to
`raw/config/config-fx10_<timestamp>.yaml`, loads that copy and records `device.json`, `seg_*.raw`
and `events.jsonl` under `raw/fx10/fx10_<UTC>/`. `device.json` is written once during Init and
holds identity, `applied`, `ebus_config`, `sensor_window`, `final_readback`, `transport`,
`runtime`, `sdk.header_version` (compile-time `NVERSION_STRING`; the runtime SDK version is not
queried), `timing` and `profile` (geometry, `pixel_format`, `full_scale`, `expected_frame_rate_hz`,
wavelengths, `status_line`, `calibration`, `shutter_request`). The configured frame rate is not
evidence of the achieved rate.

## Runtime guards and failure policy

| Condition | Result |
|---|---|
| Invalid YAML value or unknown key | Load fails before connecting. |
| Any bring-up or readback failure in steps 1–9 | Driver fails; no session is recorded as complete. |
| Missed triggers, counter regression, temperature | See the health table under [Triggering and timing](#triggering-and-timing). |
| Control link lost | Session fails; there is no reconnect. |
| SDK operation error, BlockID discontinuity, incomplete image, increase of `BlocksDropped` | Session fails; received bytes are retained. |
| Buffer pool or recording queue exhausted | Session fails; accepted buffers drain. |
| No valid frame when frames are required | Session fails. |

## Tools

### fx10_snapshot

`fx10_snapshot --config <snapshot.yaml> --out <dir>` accepts `--ip` or `--mac`, `--exposure-ms`,
`--spatial-binning`, `--spectral-binning` and `--fps`, reapplies `CheckAcquisitionTiming`, disables
size-based rotation and forces freerun: `trigger.mode: freerun`, `selector_entry: FrameStart`,
`exposure_control: camera`, SensorSync off, and final `features.raw` writes `TriggerMode = Off`,
`TriggerSelector = FrameStart`, `ExposureMode = Timed`, `AcquisitionMode = Continuous` and
`EnAcquisitionFrameRate = true`. `--frames N` (default 64) sets `max_frames = N` and
`max_duration_s = max(5, N / frame_rate_hz × 3 + 5)` in one segment. `--stream-s S` (1–3600,
exclusive with `--frames`) is the focus-assist stream: no frame limit, `max_duration_s = S` and
`max_lines = ceil(frame_rate_hz × 0.25)`, so the GUI can delete closed segments; the GUI passes
`--fps <rate> --stream-s 1800` and ends the stream with SIGTERM. Output goes to
`<dir>/fx10_<UTC>_<ns>`; no data for 10 s ends the capture. The last stdout line is
`SNAPSHOT: OK <dir>` or `SNAPSHOT: FAIL <code> <reason>`, with codes 2 (arguments or
configuration), 3 (initialization), 5 (incomplete or failed), 7 (no frame) and 130 (interrupted).

### Collect Reference page

The GUI runs `fx10_reference --config fx10_driver/config/config-fx10-snapshot.yaml --out <root>`
after the operator places an illuminated white reference. The tool reads the YAML once, forces
freerun as `fx10_snapshot` does and takes no other overrides; workbench values reach it only after
Apply writes them to the YAML. It captures a white phase (shutter opened), then a dark phase
(shutter closed), each for `reference.duration_s` with its own `EbusCameraDriver`, connection and
full bring-up. Output goes to `<root>/reference_<UTC>_<ns>/`: `reference.request.json`,
`reference.json` (`fx10-reference-v1`), `reference.log`, the loaded YAML text as
`raw/config/config-fx10.yaml`, and per phase `raw/fx10/<phase>/fx10_*/` with `device.json`,
`seg_*.raw`, `events.jsonl` and `reference_phase.json`.

Before a phase acquires, its `device.json` must hold verified readbacks of serial, model, geometry,
`PixelFormat`, binnings, `EnStatusLine`, `MROI_Enable`, `ExposureTime` and `AcquisitionFrameRate`,
with `ExposureMode = Timed`, `EnAcquisitionFrameRate = true`, `TriggerSelector = FrameStart` and
`TriggerMode = Off`. The dark signature, which also covers `applied`, the `.pvcfg` status and
SHA-256, wavelengths and calibration, must equal the white one. The manifest records
`connection_scope = "separate verified phases"`: AIE, undocumented state and shutter position are
not proven identical. A failed or interrupted phase keeps its partial recording and no successful
pair is published; the shutter stays closed after the dark phase until the next run opens it. No
SensorSync log is created, and manifests never overwrite existing files. Stdout markers are
`REFERENCE: CONFIG`, `SESSION`, `PHASE`, `OK` and `FAIL`; exit codes are 0, 2 (arguments),
3 (failure) and 130 (interrupted).

## Discovering node names

GenICam names must come from the camera, because the manual uses Lumo and ASCII names. Inspect an
existing node-map export or the feature tree in eBUS Player; the shipped `.pvcfg` is an export of
feature values. `fx10_snapshot` has no feature-dump option and always performs factory preparation
and acquisition, so it is not a read-only node dump. Before adding a `features.raw` entry, confirm
node type, access and entry spelling, in particular for AIE (`AberCorrection_Enable` in the
`.pvcfg`, Lumo `Camera.Image.AberrationCorrection.Enabled`, p. 39), the `DeviceTemperatureSelector`
entries, `Counter1_EventSource`, and `LineSelector`/`LineSource`. The `.pvcfg` names the line
entries `LineOut0`, `LineIn0`–`LineIn2` and `LED0`–`LED2`.

## Limitations

The following are not verified on hardware:

- What `CameraHeadFactoryReset` resets and which user set `UserSetLoad` loads.
- Whether the firmware keeps the detector window across a binning change; if not, every binning
  other than the file's fails the window guard.
- Whether two `.pvcfg` passes suffice, and whether their `MotorShutter_*` writes move the shutter.
- The shutter node definition, whether the firmware reads back the written pulse value, and the
  motion completion and settling time.
- Routing of `LineOut0`/`ExposureActive` to ISO_STROBE pin 2 and of `Line0` to ISO_TRIGGER pin 8.
- How the `.pvcfg` value `Counter1_AcquisitionStartReset = 1` interacts with the pre-start baseline.
- The `DeviceTemperatureSelector` entry spellings `ProcPCB` and `FPGA`.
- The operator-set rate and exposure limits, and whether the live-range clamp lowers a freerun rate.
- The unit and epoch of the device timestamp.
