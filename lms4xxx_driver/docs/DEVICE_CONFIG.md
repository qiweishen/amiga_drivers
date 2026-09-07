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
| 16 | `sRN SCdevicestate` every 200 ms, up to 10 s | wait for `01` Ready (p.46); `02` Error → messages logged, fatal | — | 124 |

The device clock is never set from the host (`LSPsetdatetime` is not sent):
the host clock is not trusted. With NTP on the device synchronises to the
configured server and the telegram carries the time stamp block; with NTP off
the clock free-runs (p.97: no RTC) and the time stamp block is switched off.

## Time

NTP is the LiDAR's **only** absolute time source, so the shipped configuration
enables it. With `ntp.enabled: false` the driver warns at start-up: the scans
then carry nothing but the device uptime (`time_since_startup_us`, wraps every
71.6 min) and cannot be associated with any other sensor offline. No host time
is recorded on this platform in any case.

* **Server.** The AsteRx RBi3 Pro+ serves NTP (and PTP) on its own address,
  10.95.2.102, on the GPS timescale. The LiDARs sit on 10.95.76.x, so the
  device itself must be able to route to that address: set the device gateway
  (`EIgate`, p.136, factory 0.0.0.0) to the host's eno1 address and enable IP
  forwarding on the host, or put the LiDARs on the AsteRx subnet. The driver
  does not write network parameters. The host-side SNTP probe only proves that
  the server is alive from the host; it says nothing about the device's route.
* **Time lock.** The LMS4xxx has no RTC (p.97): until its first NTP sync the
  time stamp block carries a free-running clock that starts at the 1970 epoch.
  The parse thread therefore records nothing until the first telegram whose
  time stamp is at or after 2026-01-01T00:00:00Z
  (`kEarliestPlausibleDeviceTimeUs`, `include/scan_verify.h`). Scans before
  that are counted (`prelock=` in the status line, `prelock_scans_discarded`
  in `drivers.json`) but not written; their telegram counters are still tracked
  so the lock does not produce a false counter gap. No plausible time stamp
  within `ntp.lock_timeout_s` of the stream start faults the run (`ntp=NO-LOCK`
  in the status line until then; `ntp=OK` once locked).
* **Step check.** Once locked, the device time must advance in step with the
  device uptime: between consecutive recorded scans
  `|Δdevice_time − Δuptime|` (uptime taken modulo 2^32 µs) above
  `ntp.max_time_step_ms` is an NTP step or a clock fault while recording and
  faults the run; the largest value seen is reported as `tstep_max_us=`. A
  device time that falls below the floor again after the lock faults as well.
* **Fail-fast.** Like every other loss (ring/queue overflow, telegram counter
  gap, checksum/framing/parse error), a time fault ends the whole rig at once —
  an incomplete recording is worthless to the platform and the operator
  restarts immediately.

After `Run`, `sEN LMDscandata 1` starts the stream. The first telegram is the
proof that everything took effect: DIST1 + RSSI1/REFL1 + ANGL1 + QLTY1, exactly
3 + 1 channels, 841 points each, step 833, start angle 550000, time stamp
block present with NTP and absent without, no encoder block, no device name
block — anything else faults the run. Scan frequency ≠ 600 Hz or a device
status ≠ ok are logged as warnings.

A latched fault now stops the parse thread immediately, including on the
telegram that raised it. It used to only set a flag that the owner polled every
200 ms, so at 600 Hz more than a hundred scans already known to be invalid were
written to the `.h5` file first.

## Shutdown

| # | Telegram | Why |
|---|---|---|
| 1 | `sEN LMDscandata 00` | stop the measured-value stream |
| 2 | `sMN SetAccessMode 03 F4724744` | `sMN Run` logged this session out at bring-up, and standby needs Authorized Client again (p.74) |
| 3 | `sMN LMCstandby` | laser off, motor keeps turning (p.74) |

All three go out while the receive and parse threads are still running, because
the answers come back as ordinary frames on the same connection — after
`ShutdownReceive()` the socket can no longer carry one. The `sAN` status bytes
are checked (**standby succeeds on 0**, see the polarity table). This used to be
two fire-and-forget writes issued while logged out, so the device almost
certainly answered `sFA 1` and **the laser kept burning after every run** (p.17:
permanent measurement shortens the laser diode's life) with nothing in the log
to say so. If the handshake fails now, the log says the laser is still on.

Timeouts: variable reads/writes use `network.response_timeout_ms` (shipped at
1000 ms; the socket's `SO_RCVTIMEO` follows it as the poll granularity, and
`SO_SNDTIMEO` is set from it so a peer that stops reading cannot block the
control thread forever). Methods (`SetAccessMode`, `mSCloadappdef`,
`LMCstartmeas`, `Run`) use at least 5 s.

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
| `sMN LMCstartmeas` / `LMCstopmeas` / `LMCstandby` | **0** | 75, 76, 75 |
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
