# Go-X device configuration

This document describes how the driver configures JAI Go-X GigE Vision color cameras and what it
records about that configuration, as implemented in the current source. Manual: JAI, *User Manual,
Gox Series - GigE Vision Interface Cameras*, version 1.3, 2025-06-16
([Manual_Go-X-Series-PregiusS-PGE.pdf](Manual_Go-X-Series-PregiusS-PGE.pdf)); printed page
numbers equal PDF page numbers.

## Scope

The driver handles one or more Go-X color cameras, one `cameras[]` entry each, on the Pleora eBUS
SDK 6.5.1 (`.devcontainer/Dockerfile` pins 6.5.1-6797). Its limits target the GOX-12405C-PGE at
full frame (4128 x 3008); the model is recorded, not checked. Configuration code lives in
`gox_driver/` and `ebus_driver/src/gox_config.cpp`; connection, stream, buffers, recording and
shutdown belong to the shared engine `ebus_driver/src/camera_driver.cpp`. Profiles are
`gox_driver/config/config-gox.yaml` (rig) and `config-gox-snapshot.yaml` (`jai_snapshot`). The
RAW format is specified in [ebus_driver/README.md](../../ebus_driver/README.md).

## Camera identity

`id` (1-63 characters of `[A-Za-z0-9_-]`, unique) names the output directory; at least one entry
must be `enabled`. A non-empty `device.mac` wins: discovery on all host interfaces (4 s) must find
exactly one device with that MAC, with an IP configuration valid for its NIC. Otherwise `device.ip`
is connected directly and the camera's identity is not checked; an entry with neither is rejected.
The driver never writes IP settings; re-addressing uses `ebus_set_ip` (GUI: Camera Tools, Set IP).
`device.json` records `connection_path`, the IP and identity nodes (model, serial, firmware, MAC).

## Bring-up sequence

Every start, `jai_snapshot` included, runs this sequence per camera. Any exception fails the
camera and, through the session runner, the whole rig.

| Step | Action |
|---|---|
| 1 | Parse YAML (strict schema); reject a `pixel_format` other than the one the run requires. |
| 2 | `PvDeviceGEV::Connect`; `AcquisitionStop` (best effort); `UserSetSelector` = Default, `UserSetLoad`. |
| 3 | If `ptp.enabled`: `GevIEEE1588` off, on, and wait for slave status. |
| 4 | Stream: socket receive buffer, `PvStreamGEV::Open`, packet size, destination, `network.receiver_tuning`. |
| 5 | Apply plan; then `GevGVSPExtendedIDMode` = On, `PixelFormat` and `SensorDigitizationBits` must read back as required. |
| 6 | Read `PayloadSize`; allocate buffers; raw audit; publish `device.json`; create `events.jsonl` and `seg_00001.raw`. |
| 7 | Register with the SensorSync hub if `sensor_trigger.enabled`. |
| 8 | Start writer and receiver threads; `CounterReset` and a baseline telemetry sample. |
| 9 | Re-check `PayloadSize`; `StreamEnable`; `AcquisitionStart`; arm SensorSync. |

The factory load (`CameraController::LoadFactoryDefaults`) restores the read-only Default set
(p. 165), so every run starts from a known state whatever a previous session or tool left. Only an
entry named "default" (case-insensitive) is accepted. `AcquisitionStop` comes first because user
sets load only while acquisition is stopped (p. 34); the load comes before PTP and the stream open
because it resets `GevSCPSPacketSize` and `GevSCPD`. During the load `AnswerTimeout` is raised to
at least 10 000 ms and `DisconnectOnAnyTimeout` cleared (both restored); `IsDone` is polled every
50 ms for up to 15 000 ms (a camera without `IsDone` is assumed done); the node cache is
invalidated and `UserSetSelector` must read back Default. `device.json` `factory_load` records the
entry, readback, elapsed time and `is_done_supported`. `UserSetSave` is never executed.

## Apply plan and readback

`BuildApplyPlan` (`gox_driver/src/apply_plan.cpp`) orders the writes by the manual's
dependencies: ID mode and margin bound the frame rate (p. 141), the A/D depth precedes the pixel
format (p. 45), and the frame rate precedes the exposure it caps. `CameraController::ApplyConfig`
executes the plan and records each request and readback in `device.json` `applied`.

| # | Feature(s) | Value | Written when |
|---|---|---|---|
| 1 | `GevGVSPExtendedIDMode` | On (64-bit block IDs; factory Off, p. 129) | always |
| 2 | `SensorDigitizationBits` | 12 | 12-bit pixel format |
| 3 | `Width`, `Height`, then `OffsetX`, `OffsetY` | ROI | value non-zero (p. 133-134) |
| 4 | `PixelFormat` | `pixel_format` (factory BayerRG8, p. 137) | always |
| 5 | `BlemishEnable` | 0, "Disable all" (p. 156) | `blemish_correction: false` |
| 6 | `NetworkThroughputSafetyMargin` | percent (p. 128) | non-zero |
| 7 | `AcquisitionFrameRate` | `frame_rate_hz` | freerun with a rate |
| 8 | `ExposureTime` | `exposure_ms` x 1000 µs | set |
| 9 | `Gain` | `gain` | set |
| 10 | `TriggerSelector`, `TriggerSource`, `TriggerActivation` (1 rising, 2 falling, p. 142), `TriggerDelay` (µs) and `OptInFilter` (ns) if non-zero, `TriggerMode` On | trigger block | external |
| 11 | `CounterSelector` 0, `CounterEventSource` 1 (FrameTrigger) | optional | external |
| 12 | `LineSelector` 21, `LineSource` 4 (ExposureActive), `LineInverter` 0 | Line2 strobe | `exposure_active_output` |
| 13 | `features.raw` entries in order | operator | always last |

Every write is read back. Plan entries are strict: integer, boolean, enum and string readbacks must
equal the request and float readbacks must lie within 1 %. `features.raw` and `receiver_tuning`
differ only in that an integer clamp or a float deviation beyond 0.1 % is logged as a warning. A
missing, unwritable or refused node fails bring-up, except the optional Counter0 entries, which
record `<absent>` or `<failed>`. A quoted `features.raw` value is written as an enum entry or
string, an unquoted one is typed by the node. The step-5 checks run after `features.raw`, so an
override of the pixel format, A/D depth or ID mode fails bring-up.

## YAML parameters

Unknown keys are errors; omitted keys keep the defaults in `gox_driver/include/app_config.h`.
"Shipped" is `config-gox.yaml`; `network.*` is listed under Network.

| Key | Range | Shipped | Notes |
|---|---|---|---|
| `acquisition.exposure_ms` | 0 < x <= 399.405 | 150.0 | See timing limits. |
| `acquisition.gain` | 1.0-126.0 | 1.0 | `Gain[AnalogAll]` magnification, not dB (p. 148). |
| `acquisition.pixel_format` | `BayerRG12Packed` | same | Also the default; other values are rejected. |
| `acquisition.frame_rate_hz` | 0.125-6.054 | 5.0 | Freerun rate, or SensorSync pulse rate (external). |
| `acquisition.blemish_correction` | bool | false | `true` keeps the factory correction. |
| `acquisition.trigger.mode` / `activation` | freerun, external / rising, falling | external / rising | |
| `acquisition.trigger.selector_entry` / `source_entry` | entry name / 7-19, 24, 36, 37 or a name | FrameStart / 24 | 24 = Line5 Opt In (p. 141). |
| `acquisition.trigger.delay_ms` / `input_filter_ns` | 0-500 / 0-40 000 000, step 100 | 0 / 0 | `TriggerDelay` (p. 142), `OptInFilter` (p. 145). |
| `acquisition.trigger.exposure_active_output` | bool | true | Line2 ExposureActive strobe. |
| `acquisition.trigger.sensor_channel` | 2, 3 | 2 | SensorSync JAI channel of this camera. |
| `acquisition.roi.*` | width 0 or >= 96 step 8; height 0 or >= 8 step 2; offsets step 8 / 2 | 0 | 0 keeps the device value. |
| `features.raw` | `[{name, value}]` | `[]` | Applied last. |
| `sensor_trigger.enabled` | bool | true | Port: `config/config-main.yaml` "Sensor Trigger: Port". |
| `ptp.enabled` / `sync_timeout_s` / `on_timeout` | bool / > 0 / abort, warn_continue | false / 10.0 / abort | `sync_timeout_s` defaults to 60. |
| `output.segment_size_mb` / `flush_interval_mb` | 1-1 048 576 | 2048 / 64 | Rotation size; `fdatasync` cadence. |
| `output.max_frames` / `max_duration_s` | >= 0 / 0-86 400 | 0 / 0 | 0 = unlimited. |
| `output.record_align` / `queue_on_full` / `on_buffer_error` | legacy | | Parsed, then superseded (failure policy). |
| `logging.stats_interval_s` | > 0 | 2.5 | `[Statistics]` cadence. |

## Settings not written

- IP configuration: owned by `ebus_set_ip`; a recording never re-addresses a camera.
- `GainSelector`, `BlackLevelSelector`: the factory load leaves them on AnalogAll and All, and the
  GO-X has no "All" gain entry (p. 148); the audit moves them only temporarily.
- Digital red/blue gains, `BlackLevel`, auto functions, LUT, color transformation, enhancers,
  shading, compression, scaling, binning, decimation, flips and test sources: the factory values
  leave the pixel path raw, so they are audited instead.
- `AcquisitionFrameRate` under an external trigger: disabled by `TriggerMode[FrameStart]` (p. 36).
- `GevSCCFGExtendedChunkData`, `ChunkModeActive`: the former refused a bring-up write ("parameter
  not writable"), and without it the SDK does not support the chunk payload type
  (`PvPayloadType.h`); both, and `GevSCPD`, are read back under `transport`.
- `SensorDigitizationBits` for the 8-bit focus stream: 8 bits quadruples sensitivity (p. 45).
- `GevIEEE1588ClockAccuracy`: it describes the camera as a potential grandmaster; it is not read.
- Requests equal to the factory value (freerun `TriggerMode`, zeros, `blemish_correction: true`).

## Pixel format and timing limits

Recording accepts only `BayerRG12Packed`: native 12-bit DN, two pixels in three bytes (PFNC
0x010C002B, `app/decoder/gox/recording.py`). `SensorDigitizationBits` = 12 precedes it because the
factory 10 bits leave gaps in the 12-bit histogram (p. 45). The only exception is `BayerRG8` for the
focus-assist stream, selectable in code by `jai_snapshot --stream-s` and never from YAML.

| Limit | Value | Basis |
|---|---|---|
| Frame rate | 0.125-6.054 Hz in every trigger mode | 6.054 Hz: operator ceiling for full-frame `BayerRG12Packed` (2026-11-08); 0.125 Hz: 8 s period (p. 36) |
| SensorSync pulse rate | 0.140-6.054 Hz | board rule in `common/include/sensor_sync_rates.h`, capped by the camera ceiling |
| Exposure | 0 < x <= 399.405 ms | `ExposureTime` maximum read back from the camera |
| Exposure vs. period | <= 0.9 x period (900 / `frame_rate_hz` ms), inclusive | freerun, and external with `sensor_trigger.enabled`; external pulses from another source are not checked |
| Focus-stream rate | <= 9.078 Hz | a 9.079 Hz write was refused (2026-11-17) |

## PTP

PTP is off in both shipped profiles. With `ptp.enabled`, `PtpManager` writes `GevIEEE1588` = false
and then true, each with readback, and polls `GevIEEE1588Status` every 500 ms (p. 128). Slave (9)
admits the camera and records `lock_wait_ms`; master (6, no grandmaster wins the BMCA) and faulty
(2, the camera's 1588 stack failed) fail immediately; any other state is polled until
`sync_timeout_s`, then fails. `on_timeout: warn_continue` turns these failures into warnings.

Once admitted, each 5 s health sample requires slave status; a bad reading is re-checked three
times, one second apart, before the session fails. The user-confirmed sole grandmaster is the
AsteRx RBi3 Pro+; the driver does not verify its identity, accuracy or timescale (`ptp.accuracy`
is always `null`). The error messages name the prerequisites: the grandmaster on the camera's L2
domain and a switch that forwards PTP multicast (224.0.1.129, 01-1B-19-00-00-00). With PTP off in
freerun the driver warns that frames carry no absolute timestamp.

## Triggering (SensorSync)

All cameras of a run share one `common::SensorSyncHub` session on the Teensy SensorSync board,
logged to `raw/sensor_trigger.log`.

| Signal | Camera | Board (`sensor_channel` 2 / 3) |
|---|---|---|
| Trigger in | Line5 Opt In, pins 2/3, `TriggerSource` 24 (p. 17, p. 141) | trigger output, GPIO 37 / 36 |
| Strobe out | Line2 Opt Out, pins 4/5, ExposureActive, high while exposing (p. 143-144) | strobe input, GPIO 14 / 15 |

- External with `sensor_trigger.enabled`: `frame_rate_hz` is required and is the commanded rate
  of 50 µs pulses. Channels 2 and 3 share one rate; each enabled camera needs its own channel. The
  YAML notes that the shipped firmware drives trigger output 2 only.
- Freerun with `sensor_trigger.enabled`: the camera registers at 0 Hz (strobes only). The hub
  refuses differing rates within a pair, so a freerun and an external camera cannot share it; the
  config loader does not detect this.
- External without `sensor_trigger.enabled`: no exposure check and no no-data watchdog.
- Pulses start once every participant has armed; over 30 s armed and waiting, a log idle for over
  15 s, or any hub failure fails the session.
- Counter0 counts received frame triggers (p. 160). It is reset before `AcquisitionStart`, and its
  binding, input (FrameStart, On, source 24), overflow (`CounterStatus` 4) and value are sampled
  every 5 s; a lost binding, regression, overflow or negative value fails the session.
- At stop, with pulses bounding the window and every counter reading valid, Counter0 minus its
  baseline must equal valid frames plus block-ID gaps, and the log must show complete exposure
  edges, one exposure per frame and one trigger per exposure. Freerun checks log integrity only.
  Because exposure edges are required, `exposure_active_output` must stay true when
  `sensor_trigger.enabled`. Frame-to-pulse association is made offline; `association_verified`
  is always `false`.

## Network

| Key | Behavior |
|---|---|
| `socket_rx_buffer_mb` (1-1024) | 32 in `config-gox.yaml`, 16 in the snapshot profile and when omitted. If the host `net.core.rmem_max` is lower, the SDK caps the request and the driver warns and records it; otherwise the readback must reach the request. |
| `packet_size` (0, 1476-12036, p. 130) | 0 negotiates, falling back to 1476 bytes with a warning. |
| `local_ip` | Binds the stream to that host address; empty lets the SDK choose. |
| `throughput_safety_margin_pct` (0, 10-100) | 0 keeps the factory 92 (p. 128); the YAML suggests about 100/N for N cameras on one NIC. |
| `receiver_tuning` | PvStream parameters, `features.raw` syntax and rules. |
| `buffer_count` (0, >= 4) | SDK buffers; 0 = clamp(ceil(rate) + 8, 8, 64), 16 without a rate. |
| `output.queue_max_frames` (0, >= 2) | Recording queue; 0 = clamp(ceil(2 x rate), 4, 64); pool = queue + 2. |

SDK buffers and pool slots share a 512 MiB payload budget (engine default, not in the Go-X YAML);
`device.json` `runtime` records the resulting counts. With an 18 625 536 B full-frame
`PayloadSize`, 28 slots fit, and at 6.054 Hz the auto queue is clamped from 13 to 11 frames.

## Dataset metadata

Rig output is `<Output Directory>/<YYYYmmdd_HHMMSS>/raw/gox/<id>/`, which must not exist yet;
`raw/config/config-gox_<timestamp>.yaml` is the copy the run loaded.

| File | Content |
|---|---|
| `device.json` | Written once before `AcquisitionStart`. |
| `events.jsonl` | One `fdatasync`ed line per event: `session_start`, `prepared`, `profile_start` (Counter0 reset, baseline), `acquisition_started`, `device_health` (start, every 5 s, stop), rejected or excluded buffers, `segment_closed`, `trigger_counter_accounting`, `stream_final_statistics`, `sensor_sync_quality`, `capture_final`, `session_end`. |
| `seg_NNNNN.raw` | Unmodified SDK payload bytes with per-record metadata. |

Besides the blocks named elsewhere, `device.json` holds `format`, `recording_format`, `sdk`,
`config_source`, `identity`, `transport` (payload and packet size, `GevSCPD`, ID and chunk modes,
frame-rate bounds), `derived` (exposure bounds, 2.45 µs exposure offset (p. 38), 8 LSB at 8 bit
black pedestal (p. 172) and the driver-derived 12-bit 128, A/D depth), `timing`, `sensor_sync`,
`profile` and `recording_contract` (device timestamp unscaled; unit and epoch unverified).

The raw audit reads 44 features the driver never writes and compares them with the documented
factory values: `match`, `deviates` (warning: recorded pixels may not be raw), `recorded` (no
documented value) or `unreadable`. Four selector-scoped reads add `Gain[DigitalRed]`,
`Gain[DigitalBlue]`, `BlackLevel[Red]` and `BlackLevel[Blue]`; a selector that cannot be restored
afterwards fails bring-up. The code selects `GainSelector` entry 2 for digital blue, whereas the
manual lists 3 (p. 148).

`device_health` rows carry sensor, mainboard and FPGA temperatures (p. 125), Counter0 value and
flags, PAUSE frames received (p. 130) and PTP status. `gox_driver/include/device_json.h` still
names a `telemetry.jsonl`; the engine writes these rows to `events.jsonl`.

## Runtime guards and failure policy

The rig is fail-fast: a camera failure terminates every driver, and `raw/drivers.json` reports the
run as failed. Host timestamps are reception diagnostics only.

| Condition | Result |
|---|---|
| SDK operation error, incomplete image, block-ID gap or anomaly | safe bytes recorded with flags; session fails |
| Unsafe payload extent; pool or queue full | buffer identity logged as an event; session fails |
| SDK `BlocksDropped` > 0, control link lost, write error, drain after stop > 5 s | session fails |
| PTP leaves slave; Counter0 fault; SensorSync failure, stall or arm timeout | session fails |
| Stop-time mismatch: Counter0, SensorSync edges, records vs. admitted buffers | session fails |
| No valid frame for `Guards: No Data Abort S` (60 s shipped, `config/config-main.yaml`) | run stops |
| Temperature >= 72 °C (p. 173), audit deviation, `features.raw` clamp, unconfirmed `CounterReset` | warning only |
| Legacy `queue_on_full`, `on_buffer_error`, `record_align` | no effect; requested and effective values in `device.json` `legacy_output_options` |

After a crash or power loss the camera directory can hold `device.json.part`, an `events.jsonl`
without `session_end`, and a last segment whose tail after the latest `fdatasync` is lost or torn;
only the CRC-valid record prefix is recoverable, and `drivers.json` still reads `running`. The next
bring-up recovers the camera with `AcquisitionStop` and the factory load, and opening the
SensorSync board sends STOP. A camera directory is never reused.

## Tools

`jai_snapshot --config <yaml> --out <dir>` (`gox_driver/tools/snapshot_main.cpp`) records
`cameras[0]` under `<dir>/<camera_id>` through the same engine, with `--ip` or `--mac`,
`--exposure-ms`, `--gain` and `--frame-rate-hz` overrides, after which it re-checks the 0.9-period
exposure rule. It forces freerun, PTP and SensorSync off and no strobe, and appends `TriggerMode`
Off, `TriggerSelector` FrameStart, `TriggerMode` Off, `ExposureMode` Timed, `AcquisitionMode`
Continuous and `GevIEEE1588` false to `features.raw`. The last stdout line is
`SNAPSHOT: OK <camera_dir>` or `SNAPSHOT: FAIL <code> <reason>` (2 arguments or config,
3 initialization, 5 incomplete or failed, 7 no frame, 130 interrupted).

| Mode | Frames | Pixel format | Rate | End |
|---|---|---|---|---|
| default | 1 | `BayerRG12Packed` | <= 6.054 Hz | first frame or 15 s |
| `--stream-s N` (1-3600) | unlimited, one per segment | `BayerRG8`, factory A/D depth (10 or 12) | <= 9.078 Hz | SIGINT/SIGTERM or N s |

Ten seconds without data end either mode. The stream feeds the GUI focus assist, which reads the
newest segment and deletes closed ones; the snapshot profile also serves the Camera workbench and
the ColorChecker reference.

## Limitations

Not verified on hardware, or not verified by the driver:

- PTP lock, grandmaster identity and timescale (shipped profiles run with PTP off); the device
  timestamp unit and epoch.
- SensorSync trigger output on channel 3; the 15 s log-stall guard at low event rates
  (`common/include/sensor_sync_log.h`: the log grows in buffered steps).
- Counter0 and SensorSync accounting over long sessions, including counter overflow.
- The raw audit's factory values on the installed firmware; the `Gain[DigitalBlue]` selector entry.
- Chunk mode off with zero chunks per frame; the achieved focus-stream rate at 9.078 Hz.
- Socket buffer capping (follows the SDK 6.5.1 headers); throughput of several cameras on one NIC.
