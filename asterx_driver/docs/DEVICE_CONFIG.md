# AsteRx device configuration

## Scope

The AsteRx driver configures and records a Septentrio AsteRx RBi3 Pro+ GNSS/INS receiver over one
TCP connection, through the vendored SsnRx SDK (Qt, `asterx_driver/3rd_party/RxTools/ssnrx`). Page
numbers refer to the AsteRx RBi3 Pro+ Firmware v1.5.2 Reference Guide
(`asterx_driver/docs/AsteRx_RBi3_Pro+_Firmware_v1.5.2_Reference_Guide.pdf`). The code is in
`asterx_driver/src/` (`commands.cpp`, `session.cpp`, `app_config.cpp`, `sbf_write_queue.cpp`,
`sbf_writer.cpp`, `asterx_driver_app.cpp`); the configuration is in
`asterx_driver/config/config-asterx.yaml`.

## Connection and login

Each attempt creates a new `SSN::SsnRx` instance, connects to `device.ip:device.port` within 5 s and
waits up to 15 s for the connection descriptor (`IPxx`), which becomes the SBF output target. The
driver logs in with `login, <user>, <password>` (p. 87) and never creates or changes accounts. The
account must have `User` access (p. 90) and be stored in `Boot`, because step 2 replaces `Current`,
including its accounts, with `Boot`.

To provision the account, send `login, <user>, <password>, RxAdmin, S3pt3ntr10` over TCP/IP to a
receiver without a full-control account; this creates `User1` and copies `Current` to `Boot`
(p. 18). Alternatively, on a COM or USB console (anonymous full control, p. 85; Ethernet-over-USB
at `192.168.3.1`, p. 15), send `setUserAccessLevel` (p. 90) and `exeCopyConfigFile, Current, Boot`.

## Configuration sequence

`BuildCommandList()` builds the sequence and `Session` sends it one command at a time, waiting up to
15 s per reply: `$R:` and `$R!` mean success, `$R?` an error (p. 62). Replies that echo another
command name are ignored, and a prompt only completes a listing whose `$R;` header has arrived, so a
late prompt or the answer to the SsnRx `\r\n` keep-alive cannot shift the sequence.

Each `set…` is followed by its `get…`; the listed fields of the first reply line that starts with
the setting name are compared case-insensitively, without quotes. Any rejection, timeout or mismatch
is a configuration failure.

| Step | Command | Purpose and readback check | Page |
|---|---|---|---|
| 1 | `login, <user>, <password>` | Authorizes the session. | 87 |
| 2 | `exeCopyConfigFile, Boot, Current` | Replaces the running configuration with `Boot`. | 68, 69 |
| 3 | `getReceiverCapabilities` | The antenna list must contain `Aux1`. | 74 |
| 4 | `setPPSParameters` · `setNTPServer` · `setPTPServer` | Time outputs; all fields compared (PPS delay 2 decimals, width 6). | 156, 155, 158 |
| 5 | `setDataInOut, <port>, , +NMEA` per distinct port · `setNMEAOutput, Stream<n>, <port>, <msgs>, <interval>` | NMEA on fixed ports; port, stream and interval compared, message list not. | 168, 193 |
| 6 | `lstAntennaInfo, Overview`, only if a type is not `Unknown` | Each configured type must equal an `ID="…"` exactly (case, spaces); the error lists up to 5 IDs sharing the first 4 characters. | 66, 107 |
| 7 | `setAntennaType, Main\|Aux1, <name>` · `setAttitudeOffset, h, p` · `setINSAntLeverArm, x, y, z` | Names quoted when they contain whitespace; all fields compared, numbers with 3 decimals. | 107, 141, 113 |
| 8 | `setCN0Mask, all, N` · `setElevationMask, all, N` · `setSatelliteTracking` · `setSatelliteUsage` · `setSatelliteHealthOverride, v, v` · `setSignalTracking` · `setSignalUsage, v, v` | Masks: value on the first echo line. Lists: presence only; `none` is compared. | 94, 110, 96, 128, 127, 100, 131 |
| 9 | `setIMUStartupDataMode` · `setIMUOrientation` | Mode compared; angles compared unless `SensorDefault`. | 173, 112 |
| 10 | `setGNSSAttitude, MultiAntenna` | Dual-antenna attitude, always on. | 142 |
| 11 | `setNtripSettings, NTR1, …` and `setNtripTlsSettings`, or `setNtripSettings, NTR1, off` | All fields except the password. | 185, 187 |
| 12 | `setPVTMode, Rover, <modes>` · `setReceiverDynamics` · `setVehicleApplication` · `setClockSyncThreshold` · `setMultipathMitigation, b, b` · `setINSNavConfig, on, all, <loc>` | Leading fields; `PVTMode` compares `Rover` only, `INSNavConfig` compares `on` and `<loc>`. | 123, 125, 135, 152, 95, 115 |
| 13 | `setDataInOut, <IPxx>, , +SBF` | Enables SBF on the driver's connection; port compared. | 168 |
| 14 | `lstConfigFile, Current` | Every line without `#` must carry a `set…` name the driver sends, or `setUserAccessLevel`; names only, not arguments. | 68 |
| 15 | `setSBFOutput, Stream<n>, <IPxx>, <blocks>, <interval>` per stream | Stream, port and interval compared; the expanded block list is not. | 202 |

Step 2 precedes every read-only step, so outputs that an earlier session left in `Current` are
removed unless `Boot` defines them. Step 14 precedes step 15 so that SBF cannot interleave with the
listing; the SDK reply limit `sMaxASCIICommandReplySize` is raised to 256 KiB for listings
(`asterx_driver/3rd_party/RxTools/ssnrx/include/ssnrx.h`). Commands over 2000 characters are
rejected before sending (p. 62).

## YAML parameters

Unknown keys and null, blank or out-of-range values are startup errors naming the dotted key.
`device.ip`, `device.user`, `device.password`, `receiver.sbf_streams`, and `id` and `blocks` (SBF)
or `descriptor` and `messages` (NMEA) per stream entry are required; omitted keys keep the defaults
given in parentheses.

| Block | Keys: accepted values (default) | Commands |
|---|---|---|
| `device` | `ip`; `port` 1–65535 (28784); `user` 1–16 characters, not `RxAdmin`; `password` 1–32 characters, of which the manual makes only half usable (p. 87); both in the charset of p. 64 without `" ' $ & ,` | `login` |
| `receiver.warmup` | `min_recording_s` ≥ 0, 0 = no duration condition (1200); `require_finetime` (true) | – |
| `receiver.time_system` | `ntp_server`, `ptp_server` (false); `pps.interval` `off`, `msec10`…`sec60` (`sec1`); `pps.polarity` `Low2High`, `High2Low` (`Low2High`); `pps.delay_ns` ±1 000 000 (0); `pps.time_scale` `GPS`, `Galileo`, `BeiDou`, `GLONASS`, `UTC`, `RxClock` (`GPS`); `pps.max_holdover_s` 0–3600 (60); `pps.pulse_width_ms` 0.000001–1000 (5) | `setNTPServer`, `setPTPServer`, `setPPSParameters` |
| `receiver.nmea_streams[]` | ≤ 10 entries, stream ID = list position (`[]`); `descriptor` `COM1–2`, `USB1–2`, `IP10–17`, `NTR1–3`, `IPS1–5`, `IPR1–5`, `LOG1–2`; `messages` from `GGA GLL GNS GST HDT RMC VTG ZDA HRP THS PASHR`; `interval` as for SBF (`sec1`) | `setDataInOut`, `setNMEAOutput` |
| `receiver.antenna` | `main_antenna_type`, `aux_antenna_type`: exact `lstAntennaInfo` ID or `Unknown`, 1–20 characters without `,` or `"` (`Unknown`); `attitude_offset_deg.heading` ±360, `.pitch` ±90, Main→Aux1 baseline relative to the vehicle axis (0); `antenna_lever_arm_m.x/y/z` ±100 m, IMU reference to main ARP, X forward, Y right, Z down (p. 113) (0) | `setAntennaType`, `setAttitudeOffset`, `setINSAntLeverArm` |
| `receiver.gnss` | `cn0_mask_dbhz` 0–60 (0); `elevation_mask_deg` −90–90 (0); `satellite_tracking`, `satellite_usage`, `satellite_health_override`: `all`, `none` or `+`-joined constellations and IDs `G01–G39` without `G37` (usage `G01–G32`), `R01–R30`, `E01–E36`, `S120–S158`, `C01–C63`, `J01–J10` (`all`, `all`, `none`); `signal_tracking`, `signal_usage`: `all` or `+`-joined signal names, constellations only for tracking (p. 131) (`all`) | `setCN0Mask`, `setElevationMask`, `setSatellite…`, `setSignal…` |
| `receiver.imu` | `startup_data_mode` `GnssTimeKnown`, `Boot` (`GnssTimeKnown`); `imu_orientation.orientation_mode` `SensorDefault`, `manual`, `fixed` (`SensorDefault`); `theta_x_deg` ±180, `theta_y_deg` ±90, `theta_z_deg` ±180 (0) | `setIMUStartupDataMode`, `setIMUOrientation` |
| `receiver.navigation` | `pvt_mode` `all` or `+`-joined `StandAlone`, `SBAS`, `DGNSS`, `RTKFloat`, `RTKFixed`, `RTK` (`all`); `receiver_dynamics` `Max`, `High`, `Moderate`, `Low` (`Moderate`); `vehicle_application` one of 17 names in `kVehicleApplications` (`Unknown`); `clock_sync_threshold` `ClockSteering`, `usec500`, `msec1`–`msec5` (`usec500`); `multipath_mitigation`, code and carrier (true); `ins_output_location` `MainAnt`, `POI1` (`POI1`) | `setPVTMode`, `setReceiverDynamics`, `setVehicleApplication`, `setClockSyncThreshold`, `setMultipathMitigation`, `setINSNavConfig` |
| `receiver.ntrip` | `enabled` (false); `port` 1–65535 (2101); `version` `v1`, `v2` (`v2`); `tls` (false); `send_gga_to_caster` `auto`, `off`, `sec1`, `sec5`, `sec10`, `sec60` (`auto`); only when enabled: `caster` ≤ 40 and `mount_point` ≤ 32 characters without whitespace, `username` ≤ 128, `password` ≤ 32, all in the charset of p. 64, empty or `<?>` rejected | `setNtripSettings`, `setNtripTlsSettings` |
| `receiver.sbf_streams[]` | ≥ 1 entry, at least one containing `ReceiverStatus` or the `Status` or `Support` group; unique `id` 1–10; ≥ 1 block or group name, not validated locally; `interval` `OnChange`, `msec5`–`msec500`, `sec1`–`sec60`, `min2`–`min60`, not `off` (`OnChange`, p. 202) | `setSBFOutput` |
| `output` | `file_prefix` (`asterx`); `live_csv` (true); `write_queue_mb` 16–4096 (256); `rotation.max_bytes` 1–2 147 483 647 (1 GiB); `rotation.max_interval_s` 1–604 800 (3600) | – |
| `logging` | `stats_interval_s` 0.001–3600 (2.5), period of the `[Statistics]` line | – |

The NTRIP password characters `" ' $ & ,` are sent as `%%DQ %%SQ %%DL %%AM %%CM` (p. 64), and the
password is redacted from logs (`RedactCmd`); the receiver account is logged in clear. NTR1 decodes
RTCMv3 in its default `auto` input mode.

## Settings not written

A configuration file lists the commands that bring the receiver from its default to its current
configuration (p. 68). A successful step 14 therefore shows that every other setting is at its
default, except permanent (`#`) commands and other instances of the driver's own command names.

| Command | Reason |
|---|---|
| `exeCopyConfigFile, Current, Boot` (p. 69) | The driver configuration is volatile; `Boot` stays under operator control. |
| Long `login` form (p. 18), `setUserAccessLevel` (p. 90) | Accounts are provisioned outside the driver; the long form could overwrite `Boot`. |
| `factoryReset` (p. 86), `exeResetReceiver, …, Config` (p. 79) | The first erases the accounts and all user settings except IP settings; the second reboots and erases `Current` and `Boot`, accounts included. |
| `lstCurrentUser` (p. 84) | A failed login already answers `$R?` (p. 87). |
| `setINSPOILeverArm` (p. 116) | The YAML has no POI geometry; `POI1` output uses the receiver default lever arm. |
| `setSignalHealthOverride`, `setTimingSystem`, `setSmoothingInterval`, `setDiffCorrUsage` | The receiver defaults (`none`, `auto`, no smoothing, `LowLatency`) apply. |
| `setIP…`, `setEthernetMode`, `setIPServices`, `setPortFirewall` | The rig reaches the receiver only by its IP; IP settings are permanent and survive step 2 (p. 21, p. 68). |

## Time outputs

The receiver is the rig's time source, so its time outputs (step 4) are written first after step 2.
The shipped YAML enables `setPPSParameters, sec1, Low2High, 0.00, GPS, 60, 5.000000`, the NTP and
PTP servers and NMEA `ZDA` on `COM1` every second; the driver verifies them only by readback.

## Warm-up gate

Rig readiness (`Session::Configured()`) requires both conditions, checked on every SBF block and
every 3 s:

- `receiver.warmup.min_recording_s` of host monotonic time since the write queue accepted the first
  nonempty SBF block; blocks received during configuration count, and nothing restarts this clock;
- if `require_finetime` is true, `RxState` bit 6 (FINETIME) of `ReceiverStatus` (block 4014,
  p. 374) in a block received after the sequence completed on the current connection.

The shipped YAML uses 90 s without FINETIME; 0 s without FINETIME opens the gate when the sequence
completes. The former key `min_uptime_s` is rejected as unknown. `AsterxDriverApp::Init()` blocks
until the gate opens, and drivers are initialized sequentially with AsteRx first (`main.cpp`,
`common/src/session_runner.cpp`), so the other drivers start after the warm-up while SBF is already
recorded. The rig-level no-data watchdog is armed when the gate opens.

## Recording

Every SBF block that SsnRx delivers is enqueued unchanged, in arrival order, from connection to
shutdown, including blocks received during configuration and blocks with unknown IDs; blocks that
arrive during backoff or shutdown are ignored.

- Files: `<session>/asterx/<file_prefix>-<YYYYMMDDTHHMMSSZ>-<n>.sbf` (host UTC at file open, `n`
  from 1). A new file starts before a block that would exceed `rotation.max_bytes`, kept below
  2 GiB because `sbf2rin` refuses larger inputs, and after `rotation.max_interval_s`; reconnects
  never start a new file. Files are flushed and closed on rotation and shutdown, without `fsync`.
- Write queue: `Session::OnSbfBlock` only enqueues on the Qt thread that reads the socket, and an
  `SbfWriteQueue` thread writes, so a disk stall does not block the reader. A block that does not
  fit in `output.write_queue_mb` is refused and the run ends.
- Live CSV (`asterx_driver/src/sbf_live_recorder.cpp`): if `output.live_csv` is true,
  `live_insnavgeod.csv` (blocks 4226, 4230) and `live_receiverstatus.csv` (4014) beside the `.sbf`
  files feed the GUI, flushed at most every 250 ms and at each link loss; a CSV failure disables
  only that file.

## Failure policy and reconnects

| Event | Before the first complete sequence | Warm-up | After the gate opens |
|---|---|---|---|
| Connect timeout (5 s), connection closed, communication error, no SBF for 3 s | fatal | reconnect | fatal |
| `$R?`, command timeout (15 s), readback, listing, antenna or capability mismatch | fatal | reconnect | – |
| SBF CRC or length error | recording incomplete | recording incomplete | fatal |
| `ReceiverStatus.UpTime` decreases (receiver reset, p. 373) | not evaluated | recording incomplete | fatal |
| Write-queue overflow or write failure | fatal | fatal | fatal |
| Recovery deadline (60 s) exceeded | – | fatal | – |

A reconnect closes the socket, waits 5 s, connects with a new SsnRx instance and re-runs the full
sequence while the write queue and current file stay open. The 60 s recovery deadline starts at the
first warm-up failure and stops only when the gate opens. After the gate opens a gap cannot be
recovered, so the session shuts down and emits `FatalError`, which stops the whole rig.

The events in the table increment `recording_errors`, also when warm-up recovers (the deadline
always follows a counted failure); bytes discarded by SsnRx while resynchronizing are only counted.
A nonzero `recording_errors` sets `known_acquisition_incomplete` in `FinalStatistics()` and marks
the driver as failed at shutdown.

## Shutdown

`Session::Shutdown()` is idempotent. It stops all timers, closes the TCP connection, drains the
write queue, joins the writer thread and closes all files; a failed final flush or close counts as a
recording error and emits `FatalError`. No command is sent, so the driver settings stay in `Current`
until the next step 2 or a power cycle, which loads `Boot` (p. 68).

## Limitations

Not verified on hardware:

- authorization and the account after `exeCopyConfigFile, Boot, Current`; after the earlier
  `RxDefault` reset the receiver answered later commands with `Not authorized!`;
- reply binding by command name, and comparing only the first line of `getCN0Mask` and
  `getElevationMask` (the GPS P-code mask is fixed at 1 dB-Hz, p. 94);
- listings over 256 KiB, lists naming constellations the receiver may not use, and
  receiver-generated lines in `Current`, each of which would fail startup;
- whether closing the socket stops the `IPxx` output, and whether the `Commands` SBF block (shipped
  stream 9) writes entered commands, including credentials, into the `.sbf` files;
- firmware compatibility (the version is not read), sustained throughput and queue headroom.
