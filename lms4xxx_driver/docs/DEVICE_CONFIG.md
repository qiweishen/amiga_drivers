# LMS4xxx device configuration

## Scope

This document describes how `lms4xxx_driver` configures, streams from, stops and records SICK
LMS4xxx 2D LiDARs (LMS4121R/LMS4124R family) over CoLa B on TCP port 2111, as implemented in
`lms4xxx_driver/` and the LMS part of `main.cpp`. One TCP connection per device carries commands
and scans; the model is read at runtime (`DIornr`, `DItype`). Page references (p. N) are to the
SICK LMS4000 operating instructions, document 8023198, edition 2024-10-25
([`operating_instructions_lms4000.pdf`](operating_instructions_lms4000.pdf)), whose telegram listing
applies to firmware V1.6 (1.6.0.0R) or newer (p. 66); PDF and printed page numbers agree.

## Instances and YAML parameters

`config/config-main.yaml` enables acquisition (`Enable LMS4XXX`) and names this file
(`LMS4XXX Driver Config Path`). `main.cpp` creates one `Lms4xxxDriverApp` per enabled `lidar[]`
entry, each with its own connection, receive thread, writer thread and, with NTP on, NTP watch
thread. All blocks except `lidar` apply to every instance. The schema is strict
(`lms4xxx_driver/src/app_config.cpp`): unknown keys are errors and omitted keys keep the defaults.

| Key | Default | Accepted | Use |
|---|---|---|---|
| `lidar[].id` | required | 1–63 chars `[A-Za-z0-9_-]`, unique | Log tag, file-name component |
| `lidar[].enabled` | `true` | bool; one must be enabled for LMS acquisition | Disabled entries get only the pre-start stop |
| `lidar[].device.ip` | required | IPv4, unique among enabled entries | Device address |
| `lidar[].device.port` | `2111` | 1–65535 | CoLa B port |
| `scan.remission` | `rssi` | `rssi`, `refl` | `RSSI1` digits or `REFL1` percent (p. 85) |
| `ntp.enabled` | `false` | bool | Device NTP client and scan time block |
| `ntp.server` | empty | IPv4, required when enabled | `TSCTCSrvAddr` and host probe target |
| `ntp.sync_interval_s` | `1` | 1–3600 | `TSCTCupdatetime` (p. 102) |
| `ntp.check_status_s` | `5` | 1–3600 | Host probe period while streaming |
| `ntp.lock_timeout_s` | `60` | 1–3600 | Deadline for a plausible device date |
| `ntp.max_time_step_ms` | `1000` | 1–60000 | Clock-step fault limit |
| `telemetry.interval_s` | `10` | 0–3600, 0 = off | Status queries while streaming |
| `output.queue_max_frames` | `512` | 2–1048576 | Receive-to-writer queue capacity, telegrams |
| `output.max_file_bytes` | `1073741824` | 0 or ≥ 1048576 | BIN split size; 0 = no splitting |
| `output.flush_interval_ms` | `1000` | 1–60000 | Periodic `fdatasync` of BIN and CSV |
| `network.recv_buffer_bytes` | `4194304` | 65536–268435456 | `SO_RCVBUF` request |
| `network.receive_thread_priority`, `_cpu` | `99`, `-1` | 1–99; −1 or 0 to CPUs−1 | Receive-thread `SCHED_FIFO` priority; CPU pin, −1 = none |
| `network.connect_timeout_ms` | `500` | 100–60000 | TCP connect deadline |
| `network.response_timeout_ms` | `200` | 100–60000 | Stream-start answer, send deadline, floor of method waits |
| `network.config_timeout_ms` | `2000` | 100–60000 | Each variable round trip during configuration |
| `network.tcp_keepalive`, `keepalive_idle_s`, `_interval_s`, `_count` | `true`, `10`, `5`, `3` | bool, 1–7200, 1–7200, 1–100 | TCP keepalive |
| `logging.stats_interval_s` | `2.5` | 0–3600, 0 = off | `[Statistics]` line period |

The shipped file enables NTP with server `10.95.76.57`, which its comment identifies as the AsteRx
RBi3 Pro+. Everything except `scan.remission` is fixed in `ScanFixed` (`include/app_config.h`).

## Configuration sequence

`Connect()` and `Configure()` (`lms4xxx_driver/src/driver.cpp`,
`lms4xxx_driver/src/driver_configuration.cpp`) run on every start. Every answer must carry the
expected type and name, a valid XOR checksum and at most 64 KiB of data.

| Step | Telegram, parameters (hex) | Required answer | Page |
|---|---|---|---|
| 1 | `sRN DeviceIdent`, `DIornr`, `DItype`, `LocationName` | none; failures do not abort | 124, 127, 131 |
| 2 | `sMN SetAccessMode 03 F4724744` (Authorized Client) | `sAN` status `01` | 72–73 |
| 3 | `sMN mSCloadappdef`, then step 2 again | `sAN` without status byte | 79 |
| 4 | Parameter rows below | `sWA`, readback | 85–112 |
| 5 | NTP on: host probe of `ntp.server` (see "Time") | healthy reply | — |
| 6 | `sMN LMCstartmeas` | `sAN` status `00` within 60 s | 75–76 |
| 7 | `sMN Run`: activate parameters, log out | `sAN` status `01` | 83 |
| 8 | `sRN SCdevicestate` every 200 ms | `01` ready within 60 s; `02` error fails | 125 |
| 9 | Audit: `OPcurtmpdev`, `ODoprh`, `ODpwrc`, `SCdevicestate`, `LMPscancfg`, `IOlasc`, `SYtype`, `SYphas` | none; stored as metadata | 73, 77, 128–130, 141–142 |

Reads need no login (p. 69). `mSCloadappdef` returns every "Application" parameter to its default
and leaves interface settings unchanged (p. 79); the manual does not state whether it logs the
client out, hence the second login. The shutdown rollback is armed before step 6, because a lost
answer does not prove that measurement stayed off. The audit warns about a laser trigger source
other than free-running, a non-zero laser timeout (p. 17, p. 77) and a non-zero `SYtype`.

| Variable | Parameters (hex) | Setting | Readback | Page |
|---|---|---|---|---|
| `LMDscandatacfg` | `01 00 07 01 uu 00 00 00 00 00 tt 00 01` | `DIST1` + remission + `ANGL1` + `QLTY1`; unit `uu` `00` RSSI, `01` REFL; no encoder or name; time block `tt` = NTP on; every scan | `sFA` tolerated | 85 |
| `LMPoutputRange` | `00 01 00000341 00086470 001312D0` | 1/12° (833 × 10⁻⁴°), 55° to 125° | strict | 86 |
| `LFPmeanfilter` | `00 0002 00` | off | strict | 103 |
| `LFPmedianfilter` | `00 0003` | off | strict | 104 |
| `LFPfrontendEdgefilter` | `00 01` | off | strict | 105 |
| `LFPedgefilter`, `LFPglossfilter` | `00` each | off | strict | 106, 112 |
| `LFPcubicareafilter` | `00 00000000 00016680 FFFF5038 0000AFC8` | off; 0 to 9.1776 m, ±4.5 m | strict | 109–110 |
| `TSCTCtimezone` | `22` | 34 = `COORD_WORLD_TIME` (UTC) | `sFA` tolerated | 100–101 |
| `TSCRole` | `01` NTP on, `00` off | client or off | `sFA` tolerated | 96 |
| `TSCTCSrvAddr`, `TSCTCupdatetime` | address bytes; uint32 s (NTP on only) | `ntp.server`, `ntp.sync_interval_s` | `sFA` tolerated | 99, 102 |

Each row is pre-read (to log the previous value), written (`sWA` required) and read back; the
readback must equal the written parameters byte for byte, and "`sFA` tolerated" accepts an `sFA`
readback answer as acknowledgement only. Any other failure aborts configuration. Variable round
trips wait `network.config_timeout_ms`; method answers wait `max(network.response_timeout_ms,
5000 ms)`, and `LMCstartmeas` 60 s because it restarts the motor after `LMCstopmeas` (p. 75).
Readback precedes `Run`, and changes take effect only after `Run` (p. 69).

### Status-byte polarity

Success is `01` for `SetAccessMode` and `Run` but `00` for `LMCstartmeas` and `LMCstopmeas`
(pp. 73–83). `LFPfrontendEdgefilter` and `LFPcubicareafilter` are written with status `00` for off,
although the manual's tables list `0 = Activate` (p. 105, p. 109); a SOPAS ET capture shows
`01` = enable and `00` = disable for both ([`sopas_filter_polarity.md`](sopas_filter_polarity.md)).
`lms4xxx_driver/tests/test_command_builder.cpp` pins the bytes.

## Settings not written

| Setting | Reason |
|---|---|
| IP address, mask, gateway, ports, CoLa dialect (pp. 132–141) | Addressing is a rig-level setting, and the application reset leaves it unchanged (p. 79). |
| `mEEwriteall` (p. 82) | Recording-relevant parameters are rewritten on each start; unsaved changes are lost at reboot (p. 72). |
| `mSCloadfacdef` (p. 78) | It deletes the entire parametrization, including interface settings. |
| Scan frequency and scan-core resolution, `LMPscancfg` (p. 73) | The manual documents only a read, and `LMPoutputRange` cannot change the resolution (p. 86); the first scan and the audit provide evidence. |
| Laser control `IOlasc` (p. 77) | Read in the audit only; unexpected values produce warnings. |
| Motor synchronization `SYtype`, `SYphas` (pp. 141–142) | The rig does not use it; `mSCloadappdef` does not reset it (p. 79), so a non-zero role is reported. |
| `LSPsetdatetime` (p. 96) | Device time comes only from NTP; host time is never written to or substituted for it. |

## Time

The device has no real-time clock (p. 97). With `ntp.enabled: false`, `TSCRole` is `00` and scans
carry no time block, only the device uptime (uint32 µs since power-up, p. 92), which wraps every
71.6 min; absolute association then needs an independently established clock mapping. With NTP on,
the device is an NTP client of `ntp.server`, every scan carries a calendar time block, and
`TSCTCtimezone 34` makes it UTC. The code treats any date before 2026-01-01T00:00:00Z as the
unsynchronized device clock (`lms4xxx_driver/include/scan_verify.h`).

The device sends NTP requests with its stored IP settings, which the driver never writes, so the
server must be reachable from the LiDAR subnet; a server in another subnet needs a device gateway
configured beforehand. The driver cannot observe that route and probes from the host instead
(`lms4xxx_driver/src/ntp_probe.cpp`): one SNTP v4 request to UDP 123, 2000 ms timeout, healthy
with version 3–4, mode 4–5, leap indicator ≠ 3, stratum 1–15, an echoed originate timestamp and a
non-zero transmit timestamp. A failed probe aborts configuration. While streaming, a watch thread
probes every `ntp.check_status_s`, retries at once after a miss and latches a time fault after
three consecutive failures. A healthy probe shows host reachability only.

The device's view comes from telemetry (`sRN EMActiveCustomerInfo`, pp. 125–126); the driver
treats message ID 38, which the manual does not list, as "No NTP signal". A warning list is fresh
for 30 s, so `telemetry.interval_s` at 0 or above 30 flags scans device-NTP-unknown for all or part
of the run.

### Time lock and clock-step checks

Every checksum-valid scan is assessed on the receive thread (`AssessScanTime`,
`lms4xxx_driver/include/clock_quality.h`). A date is plausible when well formed and not before
2026-01-01T00:00:00Z. The clock step between plausible scans is ΔUTC − Δuptime (Δuptime mod 2³²).

| Check | Threshold | Effect |
|---|---|---|
| No plausible date, including a missing time block | `ntp.lock_timeout_s` after stream start | time fault |
| Implausible date after a plausible one | any | time fault |
| \|clock step\| | > `ntp.max_time_step_ms` | time fault |
| Δuptime | 0 or ≥ 2³¹ µs | time fault |
| UTC regression, or \|clock step\| with usable uptime | > 1 ms, > 10 ms | `time_anomaly_events` |
| Any UTC regression, or \|clock step\| with usable uptime | any, ≥ 1 ms | warning log |

A time fault stops the run; the triggering scan and all earlier scans remain recorded with their
flags. Time anomalies and "No NTP signal" events do not stop the run but are final failure reasons.
No check certifies absolute accuracy.

The `ntp=` token of the `[Statistics]` line is `OFF` (NTP disabled), `NO-LOCK` (set at stream
start; latest scan has no plausible date), `UNVERIFIED` (plausible date, fresh warning list without
ID 38), `UNKNOWN` (plausible date, no complete warning list within 30 s), `NO-SIGNAL` (ID 38
present), `TIME-ANOMALY` (an anomaly occurred in this run; `NO-SIGNAL` takes precedence), `NO-TS`
(NTP on, scan without time block) or `UNREACH` (three host probes failed).

## Streaming and verification

`StartScanning()` sends `sEN LMDscandata 01` and requires `sEA LMDscandata 01` within
`network.response_timeout_ms` (pp. 90–91); a refusal or timeout runs the shutdown sequence. The
recorder starts first. A receive thread then owns the socket, reads up to 16 KiB at a time, splits
frames (`lms4xxx_driver/src/frame_receiver.cpp`) and passes each with its host receive time to the
recorder queue without blocking. It requests `SCHED_FIFO` at `network.receive_thread_priority`;
without root or `CAP_SYS_NICE` this fails with a warning and the thread runs at normal priority.

Before queuing, a monitor inspects each checksum-valid frame:

- The first scan is decoded completely and must contain exactly `DIST1`, the configured remission
  channel and `ANGL1` (16 bit) and `QLTY1` (8 bit), each with 841 points, step 833 and start 550000
  (10⁻⁴°); scale factors `DIST1` 0.1, `RSSI1` 1 or `REFL1` 0.01, `ANGL1` 1 with offset −32768,
  `QLTY1` 1; a time block exactly when NTP is on; no encoder or device-name block. A mismatch is a
  fault; a scan frequency other than 600 Hz or a non-zero device status only logs a warning.
- Later scans are read with `ParseTimestamp`, which re-reads every section length and decodes only
  counters, uptime, transmission time and calendar time. A layout error is a parse error.
- The 16-bit telegram counter must advance by one per scan; a discontinuity is logged and counted.
- Non-scan frames update the stop acknowledgement, streaming method replies and telemetry.

Telemetry writes `sRN OPcurtmpdev`, `SCdevicestate` and `EMActiveCustomerInfo` on the streaming
socket at stream start and every `telemetry.interval_s`; answers are recorded like any telegram.
The rig-wide no-data guard (`Guards:` in `config/config-main.yaml`) measures the time since the
last checksum-valid scan telegram.

## Stop measurement before rig startup

Before any driver is constructed, `main.cpp` loads this YAML without requiring an enabled entry and
calls `StopConfiguredLidars()`, also when `Enable LMS4XXX` is false; an invalid LMS YAML therefore
fails the run as `failed (configuration)` regardless of that switch. For every `lidar[]` entry in
file order, enabled or not, `lms4xxx::StopMeasurement()` (`lms4xxx_driver/src/driver.cpp`)
connects within `network.connect_timeout_ms`, sends `sMN SetAccessMode 03 F4724744` (status `01`),
then `sMN LMCstopmeas` (laser off, motor stopped, p. 76; status `00`), and closes the connection
without `Run`. The result is not evaluated: an unreachable device costs one connect timeout and is
skipped without a log entry; a command failure is logged at error level with the device IP. A
disabled entry may share an IP with another entry; that device then receives the sequence twice.
The terminate flag is checked between entries; a signal ends the run as `interrupted (signal N)`.

## Shutdown

`StopScanning()` (`lms4xxx_driver/src/driver.cpp`) runs on every exit path, including cancelled
initialization and a failed stream start. While the receive thread is alive, the commands are
written on the streaming socket and that thread collects the answers:

| Step | Telegram | Required answer | Wait |
|---|---|---|---|
| 1 | `sEN LMDscandata 00` | `sEA LMDscandata 00`; other values count as unexpected | not awaited |
| 2 | `sMN SetAccessMode 03 F4724744` | `sAN SetAccessMode 01` | 1000 ms |
| 3 | `sMN LMCstopmeas` | `sAN LMCstopmeas 00` | 1000 ms |
| 4 | `SHUT_RD`, join receive and NTP threads, report an incomplete tail | — | — |

`Run` logged the session out, and `LMCstopmeas` requires Authorized Client (p. 76); a login also
stops data output (p. 90). Without a live receive thread, steps 2–3 run as command round trips after
the workers are joined, on the existing connection when known to be idle, otherwise on a new one;
their replies are not recorded. The stop is attempted once per `LMCstartmeas`; an unconfirmed stop
is not retried and adds `device stop measurement not confirmed` to the failure reasons. A
configuration failure after the rollback is armed triggers the same stop; an earlier one sends no
`LMCstopmeas`. The writer then drains its queue and closes the split.

## Recording format

Format `lms4xxx-cola-b`, version 1 (`lms4xxx_driver/src/raw_recorder.cpp`). Each split `NNN` of an
instance is `scan_<id>_<session>_<NNN>.bin`, `.csv` and `.json` in
`<output_directory>/<session>/raw/lms4xxx/`, where `<session>` is the run timestamp
`YYYYMMDD_HHMMSS` and `NNN` the zero-based index with at least three digits. Files are created with
`O_EXCL`; an existing file fails recorder start. A split ends before a telegram that would take the
BIN past `output.max_file_bytes`; telegrams are never cut, and no empty split is created. The YAML
snapshot is also `raw/config/config-lms4xxx_<session>.yaml`. Live code decodes points only for the
first-scan check; point decoding, scaling and HDF5 export belong to `lms4xxx_decoder` in
PhenomateDecoders, outside this repository.

The BIN holds every complete telegram extracted while streaming, byte for byte in reception order:
`02 02 02 02`, a big-endian uint32 data length (1–65536), `<type> <name> <parameters>` and the XOR
checksum (p. 66). This covers `sSN LMDscandata` scans, replies received while streaming (`sEA
LMDscandata 00`, telemetry `sRA`, shutdown `sAN`, `sFA`) and telegrams with a failed checksum, each
once. It excludes host commands, command-phase replies (including `sEA LMDscandata 01`), bytes
rejected by framing and an incomplete tail at stop; those count as framing errors only.

| CSV column, in header order | Meaning |
|---|---|
| `offset` | Byte offset of the telegram in the split's BIN |
| `length` | Telegram bytes including STX, length and checksum |
| `host_receive_monotonic_us` | Host `steady_clock` µs after frame assembly; diagnostic, never scan time |
| `is_scan` | `1` for a checksum-valid `sSN` or `sRA LMDscandata` telegram |
| `checksum_valid` | `1` when the XOR checksum matches |
| `device_time_unix_us` | Time block as Unix µs, no leap-second table; `0` if absent or malformed |
| `clock_step_us` | ΔUTC − Δuptime to the previous plausible scan; `0` if not computable |
| `clock_quality_flags` | Bit set below; `0` = not assessed (non-scan, bad checksum, parse error) |

Flags: `0x001` assessed, `0x002` no plausible date, `0x004` first plausible sample, `0x008` UTC
repeated, `0x010` UTC backwards, `0x020` step above `ntp.max_time_step_ms`, `0x040` NTP disabled,
`0x080` device reports no NTP signal, `0x100` device NTP state unknown, `0x200` uptime
discontinuity, `0x400` absolute time unverified (every assessed scan).

The JSON keys are, in order, `format`, `format_version`, `instance_id`, `session_timestamp`,
`split_index`, `metadata`, `frames_total`, `scans_total`, `bytes_total`, `closed_cleanly`.
`metadata`, identical in every split, holds the device identity (`device_firmware`,
`device_order_number`, `device_type`, `device_name`, `device_keeps_flagged_points`, true for order
1116198, LMS4124R-13000S01), `remission`, the YAML text as `config_yaml`, `ntp_enabled`,
`max_time_step_ms`, the fixed geometry (`start_angle_deg`, `stop_angle_deg`, `angle_step_deg`,
`output_rate`), `absolute_time_verified` (`false`) and `audit`, the answered step-9 values.

A split opens with its JSON written as `closed_cleanly: false`, then the BIN and the CSV header,
and the files and the directory are synced. BIN and CSV receive `fdatasync` every
`output.flush_interval_ms`. On rotation or stop, BIN and CSV are synced and closed, and the JSON is
rewritten with final counts and `false`, then, only if no error occurred, with `true`. This is not
an atomic transaction: after a crash a BIN may hold bytes beyond the last CSV row or a truncated
last telegram, and the in-place JSON rewrite may be torn. A split is complete only when
`closed_cleanly` is `true`, the BIN size equals `bytes_total`, the CSV row count equals
`frames_total`, and the instance's verdict in `raw/drivers.json` has no failure.

## Runtime guards and failure policy

`Lms4xxxDriverApp::Run()` (`lms4xxx_driver/src/lms4xxx_driver_app.cpp`) polls every 200 ms. After a
stop request the receive thread keeps archiving complete telegrams until shutdown.

| Condition | Online effect | Final verdict |
|---|---|---|
| TCP read error or EOF; 1000 consecutive framing errors; first-scan mismatch; time fault; three failed NTP probes | fault, run stops | failure |
| First checksum, framing or parse error; queue full; writer I/O error | run stops (fail-fast) | data-integrity failure |
| Telegram counter discontinuity | warning, `gaps` counter | data-integrity failure |
| Received, queued and written telegram counts differ | — | data-integrity failure |
| Time anomaly or device "No NTP signal" | counters | time-quality failure |
| `LMCstopmeas` not confirmed | — | failure |
| Decoded reply that the driver does not model | `unexpected` counter | — |
| No scan telegram for `No Data Warn S` / `No Data Abort S` | warning / run stops | rig guard |

Any failure reason marks the instance failed; `raw/drivers.json` carries its final statistics,
including `failure_reasons`, `data_integrity_failed`, `time_quality_degraded`,
`stop_measurement_confirmed` and `recording_incomplete`. `drop_ring` and `prelock` are legacy
statistics that remain 0. From the fixed layout, a scan telegram with time block is 6059 bytes; at
600 Hz an instance writes about 3.6 MB/s, 512 queued telegrams absorb about 0.85 s of writer stall,
and a 1 GiB split holds about 5 min.

## Limitations

Not verified on hardware:

- whether `mSCloadappdef` logs the client out or resets scan frequency, scan-core resolution,
  `IOlasc` and `LMCminValidMeasPointOutput` (p. 88), none of which the driver writes;
- whether `LMDscandatacfg` and the `TSC*` variables answer `sRN` with data or with `sFA`;
- the filter state after `Run`, and the filter polarity on an LMS4121R (the capture used an
  LMS4124R-13000S01);
- the meaning of message ID 38, and whether the device's NTP client reaches the server;
- the instant within a scan that the time block refers to, and the absolute accuracy of device time;
- whether a login left by the pre-start stop ends with its TCP connection;
- the effective `SO_RCVBUF`, `SCHED_FIFO` availability and the data-rate figures above.
