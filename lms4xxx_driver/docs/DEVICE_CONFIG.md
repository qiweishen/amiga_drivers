# LMS4xxx device configuration

On every start the driver resets the SICK LMS4000's application parameters to
their factory values, then writes and reads back the few parameters the
recording depends on, so that a setting saved on the device by a previous
user can never influence a recording. Page numbers refer to the operating
instructions 8023198/1MNR/2024-10-25 (`docs/operating_instructions_lms4000.pdf`,
annex 12.3 "Telegram listing").

Goal: the most complete raw data the device can deliver — full aperture, full
resolution, every scan, every channel, no filter, no correction, no trigger.

## Rules

* **Baseline = `sMN mSCloadappdef`** (p.79): "deletes only the user
  parametrization of the parameters under the header Application. Other
  parameters like Interface settings, etc. remain unaffected." Verified on
  the device: IP, ports and the other communication settings survive. The
  telegram is answered by `sAN mSCloadappdef` without a status byte.
* **No factory reset, no reboot.** `mSCloadfacdef` (p.78) "deletes the entire
  parametrization of the device" including the network settings; `mSCreboot`
  (p.83) is not used either. The platform reaches the device only by its IP.
* **Network parameters are never written**: `EIIpAddr`, `EImask`, `EIgate`,
  `EIHstPort`, `EIUDPPort`, `EIHstCola`, `EIUDPCola` (pp.132–141). Neither are
  the LED function, the passwords, or `LocationName`.
* **Volatile configuration.** `sMN mEEwriteall` is deliberately not sent: the
  loaded defaults and the written values are active after `sMN Run` for this
  session; after a power cycle the device returns to its stored state until
  the driver runs again (p.72, p.82).
* **Belt and braces.** The application defaults already have every filter off
  and the full aperture at full resolution; the driver still writes those
  parameters explicitly and byte-compares the readback (`sWN` → `sWA`, then
  `sRN` → `sRA` equal to the bytes written; "every write command has a read
  counterpart", p.69). A mismatch is fatal. Rows marked *tolerate-sFA* only
  tolerate a device that answers `sFA` on the `sRN`; their effect is verified
  independently (first telegram content, NTP probe).
* Each parameter is pre-read before the write; a `(was [..])` in the log line
  means the value after `mSCloadappdef` differed from what the driver writes —
  i.e. an application default that is not "off/full". Expected: none.

## Sequence

Reads before login (informational, `sRN`): `DeviceIdent` (firmware, p.124),
`DIornr` (order number, p.127; `1116198` = LMS4124R-13000S01), `DItype`
(p.127), `LocationName` (p.131). They are logged and stored in the HDF5 root
attributes (`device_*`, `FORMAT_H5.md`).

| # | Telegram (parameter bytes) | Value | Readback | Page |
|---|---|---|---|---|
| 1 | `sMN SetAccessMode 03 F4724744` | Authorized Client login (laser off until Run) | `sAN 01` | 72 |
| 2 | `sMN mSCloadappdef` | application defaults; interface settings untouched | `sAN` (no status byte) | 79 |
| 3 | `sMN SetAccessMode 03 F4724744` | login again (whether the reset logs the client out is undocumented) | `sAN 01` | 72 |
| 4 | `sWN LMDscandatacfg 01 00 07 01 UU 00 00 00 00 00 TT 00 01` | DIST + remission + ANGL + QLTY, `UU` = 00 RSSI / 01 REFL, encoder off, device name off, `TT` = time stamp 01 with NTP / 00 without, every scan | tolerate-sFA (first telegram) | 84 |
| 5 | `sWN LMPoutputRange 00 01 00000341 00086470 001312D0` | 1/12°, +55° … +125° (841 points) | strict (documented `sRA`, p.87) | 86 |
| 6 | `sWN LFPmeanfilter 00 0002 00` | mean filter off | strict | 103 |
| 7 | `sWN LFPmedianfilter 00 0003` | median filter off | strict | 104 |
| 8 | `sWN LFPfrontendEdgefilter 00 01` | downstream edge filter off | strict | 105 |
| 9 | `sWN LFPedgefilter 00` | upstream edge filter off | strict | 106 |
| 10 | `sWN LFPcubicareafilter 00 00000000 00016680 FFFF5038 0000AFC8` | rectangular filter off (box 0…9.1776 m, ±4.5 m) | strict | 109 |
| 11 | `sWN LFPglossfilter 00` | gloss compensation off | strict | 112 |
| 12 | `sWN TSCTCtimezone 22` | 34 = COORD_WORLD_TIME: device timestamps in UTC | tolerate-sFA | 100 |
| 13 | NTP on (shipped default): `sWN TSCRole 01`, `sWN TSCTCSrvAddr a b c d`, `sWN TSCTCupdatetime <s>`; NTP off: `sWN TSCRole 00` | per `ntp:` in the yaml; the server is probed with an SNTP query and watched while scanning; the device time itself is checked by the time lock (see "Time" below) | tolerate-sFA | 96–102 |
| 14 | `sMN LMCstartmeas` | laser and motor on | `sAN 00` | 75 |
| 15 | `sMN Run` | activates all parameters, logs out, laser on | `sAN 01` | 83 |
| 16 | `sRN SCdevicestate` with 200 ms between polls, 60 s polling window | wait for `01` Ready (p.46); `02` Error → messages logged, fatal | — | 125 |

The device clock is never set from the host (`LSPsetdatetime` is not sent):
the host clock is not trusted. With NTP on the device synchronises to the
configured server and the telegram carries the time stamp block; with NTP off
the clock free-runs (p.97: no RTC) and the time stamp block is switched off.

## Time

NTP is the LiDAR's **only** absolute time source, so the shipped configuration
enables it. With `ntp.enabled: false` the driver warns at start-up: the scans
then carry nothing but the device uptime (`time_since_startup_us`, wraps every
71.6 min). Absolute association then requires an independently established
mapping. Host monotonic reception is recorded for diagnostics, never substituted
for device scan time.

* **Server.** The AsteRx RBi3 Pro+ serves NTP (and PTP) on its own address,
  10.95.2.102, on the GPS timescale. The LiDARs sit on 10.95.76.x, so the
  device itself must be able to route to that address: set the device gateway
  (`EIgate`, p.136, factory 0.0.0.0) to the host's eno1 address and enable IP
  forwarding on the host, or put the LiDARs on the AsteRx subnet. The driver
  does not write network parameters. The host-side SNTP probe only proves that
  the server is alive from the host; it says nothing about the device's route.
* **Plausibility.** The date floor is 2026-01-01T00:00:00Z
  (`kEarliestPlausibleDeviceTimeUs`, `include/scan_verify.h`). A date above it
  never means verified NTP lock. Pre-plausibility scans are now written with
  invalid-time flags; the legacy `prelock_scans_discarded` counter stays zero.
  No plausible timestamp within `ntp.lock_timeout_s` still requests a time-fault stop.
* **Device health.** Device warning 38 (No NTP signal) produces `NO-SIGNAL`,
  independently of host NTP probes. Missing, malformed, or >30-second-old warning
  readbacks produce `UNKNOWN`. A clear fresh warning list yields `SYNCHRONIZED`,
  not an accuracy guarantee. Disabling telemetry leaves device health unknown.
* **Step check.** Once locked, the device time must advance in step with the
  device uptime: between consecutive recorded scans
  `|Δdevice_time − Δuptime|` (uptime taken modulo 2^32 µs) above
  `ntp.max_time_step_ms` is an NTP step or a clock fault while recording and
  faults the run; the largest value seen is reported as `tstep_max_us=`. A
  device time that falls below the floor again after first plausibility faults
  as well. Every UTC regression is separately flagged/counted even below this
  threshold; equal UTC samples are counted without being mistaken for lost scans.
  Regressions and device NTP-loss reports degrade the final session verdict.
  `ntp_device_loss` counts entering the device's No NTP signal warning state,
  including the first such report after starting; it does not prove a previously
  locked clock lost lock. A later recovery does not erase an event from the run.
* **Time-fault stop.** A severe time fault requests orderly rig shutdown. The
  triggering scan and queued valid scan payloads remain recordable with their
  original timestamp and quality flags. No synthetic corrected time is written.

The first UTC-regression warning in each acquisition includes `utc_offset_us`
(current UTC minus previous UTC, also shown in milliseconds), `uptime_delta_us`
(the unsigned modulo-2^32 uptime delta), and `clock_step_us`
(`utc_offset_us - uptime_delta_us`). If uptime is discontinuous, `clock_step_us`
is logged as `n/a`; the UTC offset is still reported. Previous/current Unix UTC
microseconds and the current `scan_counter` locate the event in the recording.
For example, a 19 ms UTC regression while uptime advances by 1644 us produces
`utc_offset_us=-19000` and `clock_step_us=-20644`. These describe changes between
scans, not an offset measured against the NTP server or another sensor. The
existing first-warning-only policy is retained; all regressions are still
counted in `utc_back` and flagged in the HDF5 scan metadata.

After `Run`, `sEN LMDscandata 1` starts the stream. The first telegram is the
proof that everything took effect: DIST1 + RSSI1/REFL1 + ANGL1 + QLTY1, exactly
3 + 1 channels, 841 points each, step 833, start angle 550000, time stamp
block present with NTP and absent without, no encoder block, no device name
block — anything else faults the run. Scan frequency ≠ 600 Hz or a device
status ≠ ok are logged as warnings.

Protocol/content faults stop parsing. Time faults instead allow the parser to
drain scans and shutdown replies while the owner stops the stream; the writer
drains before final statistics are published.

## Stop measurement before rig startup

Before constructing any sensor driver, `main.cpp` loads the LMS configuration
selected by the main config, snapshots it, and visits every `lidar[].device`
IP/port. This includes disabled entries and runs even when `Enable LMS4XXX` is
false. All entries may be disabled for this preliminary stop; enabling LMS
acquisition still requires at least one enabled entry.

Each address gets one connection attempt using `network.connect_timeout_ms`.
An unreachable device is silently skipped. A connected device receives
`sMN SetAccessMode 03 F4724744` and, after successful login, `sMN LMCstopmeas`.
Authorized Client access is required; the stop command shuts off the laser and
stops the motor (manual p.76, tables 35–38). Its successful response is
`sAN LMCstopmeas` with status `0`. Command waits remain bounded by the existing
method timeout, at least 5 s per response read; connected-device command errors
remain diagnostic and do not prevent trying the remaining addresses or starting
the rig. No configuration reset, `Run`, or measurement-start command is sent.
The temporary connection is released before moving to the next device. A
shutdown request ends the loop before the next device and prevents driver startup.
An invalid or unreadable LMS configuration is still a configuration error.

## Shutdown

| # | Telegram | Why |
|---|---|---|
| 1 | `sEN LMDscandata 00` | stop the measured-value stream |
| 2 | `sMN SetAccessMode 03 F4724744` | `sMN Run` logged this session out at bring-up; stopping measurement requires Authorized Client again (p.76) |
| 3 | `sMN LMCstopmeas` | shut off the laser and stop the motor (p.76) |

During normal acquisition all three go out while the receive and parse threads are still running, because
the answers come back as ordinary frames on the same connection — after
`ShutdownReceive()` the socket can no longer carry one. The `sAN` status bytes
are checked (**LMCstopmeas succeeds on 0**, see the polarity table). Failed login,
rejected commands, connection errors and missing answers leave measurement stop
unconfirmed; they do not establish the laser or motor state.

The cleanup obligation is armed before sending `LMCstartmeas` in `Configure()`:
the command can take effect even if its answer is lost. Configuration failure,
cancellation before streaming, writer startup failure and partial worker startup
therefore use the same idempotent cleanup transaction. Before streaming, a healthy
command channel performs login and stop measurement synchronously. If the command channel
is uncertain, streaming may have started without its acknowledgement, or the
worker set cannot decode answers, cleanup joins all old workers first, closes the
old connection, and makes one bounded connection attempt for login and stop measurement
only. It does not reconfigure the device or restart measurement. The cleanup
result is retained across repeated `StopScanning()` and `Disconnect()` calls;
failure is reported rather than silently retried or converted to success.

After the stop handshake, the receive loop is stopped and joined, the parser
drains its queue and joins, and the writer drains and closes its file. EOF caused
by the local receive shutdown is a trace-level cancellation, not an unexpected
connection-loss warning. A remote EOF or connection reset during acquisition
still reports an error.

Thread/resource teardown and the recording verdict are separate. A run with a
device No NTP signal event still fails the existing time-quality policy even if
every accepted scan was written and every thread stopped. Its final log now says
`LMS4xxx shutdown complete; recording is INCOMPLETE:` followed by the reasons
(for example `device reported No NTP signal: ntp_device_loss=1`). This remains a
nonzero run exit status; stopping via a signal must not erase recorded faults.
Successful-session markers keep their existing meaning.

The LMS4xxx entry in `drivers.json` adds these statistics:

* `shutdown_complete`: the receive/parse/writer teardown and disconnect returned.
  This describes resource teardown, not whether stopping measurement succeeded on the device.
* `measurement_start_requested`: `LMCstartmeas` was about to be sent; this does
  not assert that its answer arrived or that measurement actually started.
* `measurement_cleanup_attempted`, `stop_measurement_command_attempted`,
  `stop_measurement_confirmed`: distinguish attempting cleanup, sending `LMCstopmeas`, and
  receiving its successful answer. No requested measurement leaves all
  three false. `measurement_cleanup_error` retains an unsuccessful cleanup's
  error; an unconfirmed required measurement stop fails the session verdict.
* `data_integrity_failed`: observed receive/parse/writer loss, a writer failure,
  or disagreement between validated, queued and written scan totals after drain.
* `time_quality_degraded`: an observed time-quality event or a bad final NTP
  status. Before the time grace period expires, a bad final status alone is
  diagnostic; it does not add a new failure policy.
* `failure_reasons`: the explicit reasons for rejecting this recording, including
  driver faults and earlier run failures. `recording_incomplete` remains the
  aggregate verdict for compatibility.

False degradation/error flags do not certify end-to-end completeness or absolute
time accuracy; `absolute_time_verified` remains false.

`frames`/`frames_received` still count all complete CoLa B frames. The new
`control_frames` log field (`non_scan_frames` in the manifest) counts frames
decoded and classified as non-scan replies. `scan_candidates` is their difference;
undecodable, dropped and unprocessed complete frames remain in this denominator.
`parsed` now counts scans that passed parsing and configured-content checks.
`delivery` is `parsed / scan_candidates`; it is not a wire-loss or disk-success
rate. CRC/framing errors and missing telegrams retain their separate counters.
The live `rate` counts validated scans per second; `fps` still counts written scans.

Timeouts: configuration variable reads/writes use `network.config_timeout_ms`
(shipped at 2000 ms); scanning-time access uses `network.response_timeout_ms`
(200 ms). Nonblocking socket I/O enforces read/write deadlines; the kernel
`SO_RCVTIMEO`/`SO_SNDTIMEO` settings are defensive, not the poll granularity.
Command-phase methods use at least 5 s per response read, except `LMCstartmeas`,
which uses at least 60 s. Larger `network.response_timeout_ms` values are retained.
This longer startup allowance does not apply to login, `Run` or `LMCstopmeas`.

After a full `LMCstopmeas`, starting measurement includes restarting the motor
(p.75). The 60 s startup allowance and the separate 60 s Ready polling window are
host timeout policies, not manufacturer timing guarantees: the manual does not
specify the maximum `LMCstartmeas` reply latency. Only a successful `sAN` permits
`Run`, followed by the Ready check. The startup reply and Ready elapsed times are
logged separately; a timeout still fails configuration and triggers stop cleanup.

The response header and body each have a read deadline, so a partially delivered
reply can take two such waits. The Ready window is checked between transactions
and may be exceeded by the final read. Configuration I/O is synchronous and does
not observe the application's stop predicate mid-command; signal cancellation can
therefore wait for the current command/readiness check before cleanup begins.

A command-phase timeout is fatal, and it now also *realigns*: `TCPClient::Read`
returns the bytes it already took off the wire instead of reporting 0, and the
caller drains whatever is still queued. Without that, one slow answer left the
stream mid-frame and every later command read the previous answer's tail as its
own header — a single 200 ms miss wedged the control channel for the whole run.

### Status-byte polarity is NOT uniform

The `sAN` status byte means the opposite thing depending on the command, which
is an easy place to write a check that always passes:

| Telegram | Success | Page |
|---|---|---|
| `sMN SetAccessMode` | **1** | 73 |
| `sMN Run` | **1** | 83 |
| `sMN mEEwriteall` | **1** | 82 |
| `sMN LMCstartmeas` / `LMCstopmeas` | **0** | 75, 76 |
| `sMN mSCloadappdef` / `mSCloadfacdef` | *no status byte at all* | 78, 79 |

### Reads before login are best-effort by design

`DeviceIdent`, `DIornr`, `DItype` and `LocationName` only warn on failure: they
are provenance, not configuration, and a firmware that does not answer one of
them must not cost the recording. Everything from `SetAccessMode` onwards is
fatal.

### sFA tolerance

A row marked *tolerate-sFA* tolerates the device refusing to *read the variable
back*, on both the pre-read and the readback, and for every SOPAS code including
1 (wrong user level). The two used to disagree: the pre-read accepted `sFA 1`
while the readback treated it as fatal, so a row could fail exactly where its own
policy said it should not.

## Device self-report (read, never written)

After the configuration is live the driver asks the device about itself and
stores the answers as HDF5 root attributes; while streaming it repeats a subset
every `telemetry.interval_s` into the `/telemetry` group (`FORMAT_H5.md`).
Queries need no user level (p.69), so this works with the session logged out.
The control channel and the data stream share one TCP connection and the receive
thread owns the socket, so a telemetry poll only *writes* the `sRN`; the answers
return as ordinary frames and are decoded on the parse thread.

| Read | Why | Page |
|---|---|---|
| `sRN OPcurtmpdev` | internal temperature; the device is specified for −10…+50 °C ambient and an outdoor rig has no other thermal record | 130 |
| `sRN ODoprh`, `sRN ODpwrc` | operating hours and power-on cycles — the laser diode's life ledger (p.17) | 128, 129 |
| `sRN SCdevicestate` | 0 busy / 1 ready / 2 error | 124 |
| `sRN EMActiveCustomerInfo` | active warnings and errors in clear text (e.g. ID 45, "No synchronisation signal") | 125 |
| `sRN LMPscancfg` | scan frequency, angular resolution and aperture **before any filter** — independent evidence of the geometry, separate from what `LMPoutputRange` asked the output stage for | 73 |
| `sRN IOlasc` | must be trigger source 0 (free-running), which continuous output requires (p.17), and laser timeout 0; anything else is a warning because a timeout would switch the laser off part-way through a survey | 77 |
| `sRN SYtype`, `sRN SYphas` | motor synchronisation — see below | 141–142 |

### Motor synchronisation is read but never written

This rig does not use it: two devices sit side by side and a third points
vertically down, so no device's beam enters another's window, and the layout does
not meet the manual's "the devices must have the same orientation" precondition
(p.30) anyway.

It is still *read*, because `SYtype`/`SYphas` are documented under **Interfaces**
(12.3.1.4.9) and `mSCloadappdef` explicitly leaves interface parameters alone
(p.79). A primary/secondary role set once in SOPAS therefore survives every
restart, every power cycle and every application reset this driver performs. A
non-zero role is logged as a warning and recorded in the file, so the state is at
least visible.

## Covered by the application defaults, no longer written

`LMCminValidMeasPointOutput` (object trigger, p.88), `LFPcubicAreaRotationAngle`
(p.111), `LFPedgefilterNearFarDist` (p.108), `LICencset` (encoder, p.114),
`LFPglossAreaMaxBeamCount` (p.113), `LFPedgefilterMaxDist` (deprecated), and
`IOlasc` (laser free-running, p.77 — the factory default per p.20, and now
read back rather than assumed). Their builders remain in
`lms4xxx_command_builder.h` and are tested against the manual's frames.

`SYtype`/`SYphas` are **not** in this list: they are Interfaces parameters and
the application reset does not touch them (see above).

## Not written, and why

| Parameter | Reason |
|---|---|
| `LocationName` (p.131) | Maintenance level; the device name is recorded as it is |
| `mEEwriteall`, `mSCreboot`, `mSCloadfacdef` | persistence / reset — see *Rules* |
| `EI*`, `HMIfpFcn_Y1`, `SetPassword` | network, LED, passwords |
| `LSPsetdatetime` | host clock not trusted |

## What the yaml exposes

Only `scan.remission` (`rssi` digits or `refl` calibrated percent — the `Unit`
byte of `LMDscandatacfg`; **the device streams one remission channel or the
other, never both**, so this is a choice, not a switch you can leave off), plus
the `ntp`, `network`, `output`, `telemetry` and `logging` sections.

`lms4xxx::LoadAppConfig` (`include/app_config.h`, on the shared
`common/include/config_util.h` primitives) validates a **strict schema**: every
block is a closed set and an unknown key is a startup error naming the key and
the accepted set.
That is what makes the paragraph above true — the former `scan.enable_*`,
`start/stop_angle_deg`, `angular_resolution_deg` and `output_rate` keys, and any
typo, are rejected rather than silently ignored. It also rejects a `scan:`,
`network:` or `ntp:` block nested under a `lidar[]` entry: those settings are
process-wide, and accepting one there would look like a per-device override that
never happened. Every numeric key is range-checked (`tests/test_config_schema.cpp`
covers each bound); the values that reach a syscall or a thread attribute —
`recv_buffer_bytes`, `receive_thread_priority`, `receive_thread_cpu`, the three
timeouts (`config_timeout_ms` covers the ~13 SOPAS round trips of `Configure()`),
the keepalive triple — are all bounded.

`telemetry.interval_s` sets the device-poll period (see *Device self-report*).

There is no `watchdog:` block: the "device holds TCP open but stops streaming"
case is caught by the rig-wide no-data watchdog in `config/config-main.yaml`
(`Guards: No Data Abort S`), which polls every driver through
`IDriverApp::MicrosSinceLastData()`. A config that still carries the old block
is rejected by the strict schema.

## Resolved: status polarity of the edge and cubic filters

Tables 130 and 146 print "Activate: 0 / Deactivate: 1", and the manual then
contradicts itself: its own worked example on p.110 activates the cubic filter
with `sWN LFPcubicareafilter 1 +10000 ...`. A SOPAS ET capture of the frames the
tool actually sends settled it (`docs/sopas_filter_polarity.md`):

```
Enable  LFPfrontendEdgefilter  ... 20 01 01 1D   Disable ... 20 00 01 1C
Enable  LFPcubicareafilter     ... 20 01 00 ...  Disable ... 20 00 00 ...
```

so **01 = enable, 00 = disable** for both — the driver's
`ScanFixed::kFrontendEdgeFilterOff = kCubicAreaFilterOff = 0x00` is right and
the tables are mislabelled. Do not "correct" this back to the tables.

## Remaining assumptions

* **`sRA` layouts** of the filter variables are not printed; the byte-compare
  assumes the SOPAS symmetric serialization that `LMPoutputRange` already
  proves on this device. A readback mismatch prints both hex strings.
* **Standard devices invalidate points flagged by quality bits 1–4** (p.22);
  only the LMS4124R-13000S01 keeps them. Firmware behaviour, not configurable;
  `device_keeps_flagged_points` in the HDF5 file records which one was used.
