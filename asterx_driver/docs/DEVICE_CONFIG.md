# AsteRx receiver configuration

On every connection (first start and every reconnect before recording) the
driver resets the receiver's running configuration to the factory defaults,
applies exactly what
`config/config-asterx.yaml` lists, verifies it, and starts recording only once
the receiver has warmed up. A setting left behind by a previous user can never
influence a recording. Page numbers refer to the AsteRx RBi3 Pro+ Firmware
v1.5.2 Reference Guide (`docs/AsteRx_RBi3_Pro+_Firmware_v1.5.2_Reference_Guide.pdf`).

## Rules

* **Baseline = `exeCopyConfigFile, RxDefault, Current`** (p.69). A
  configuration file is "the list of user commands needed to bring the
  receiver from its default configuration to a certain non-default
  configuration" (p.68); copying `RxDefault` into `Current` empties that list.
  It is the **second** command of the sequence, before any read-only step: the
  previous session's `setSBFOutput … IPxx` lines are still armed on the
  connection descriptor we just received, so SBF would otherwise interleave
  with the listings.
* **IP settings survive.** Permanent commands are "not erased even after a
  reset to default configuration" and are listed with a leading `#` by
  `lstConfigFile` (p.68). `setIPSettings`, `setIPPortSettings`,
  `setEthernetMode`, `setDynamicDNS` and `setUSBInternetAccess` carry that
  property (p.21, p.70, p.176, p.180). The command port (28784) is therefore
  untouched and the TCP session stays up.
* **Accounts do not survive.** `setUserAccessLevel` (p.90) is ordinary
  configuration content (p.18 persists the first account with
  `exeCopyConfigFile, current, boot`; `factoryReset` p.86 lists "known users"
  among the erased user settings), and anonymous users cannot send commands
  over TCP/IP (p.15, p.87). Observed on hardware: after the reset every
  command answers `$R? … Not authorized! …` — the accounts *and* the session
  authorisation are gone. The driver therefore sends the login again right
  after the reset; the long form re-creates the account (`User1`) and
  re-authorises the session in one step (p.18).
* **One login form.** "The receiver will always accept the long version of the
  login command, but the RxAdmin and S3pt3ntr10 arguments are considered only
  if there are no user accounts with full-control rights. In all other cases,
  they are ignored" (p.18). `login, <user>, <password>, RxAdmin, S3pt3ntr10`
  therefore covers both a provisioned and a bare receiver, and the driver needs
  no error fallback.
* **Volatile, except the account.** The driver never copies anything to
  `Boot` itself, but the login after the reset does (p.18): `Boot` becomes
  "defaults + the driver account" on every bring-up. Everything the driver
  configures afterwards stays volatile: after a power cycle the receiver
  returns to that `Boot` until the driver connects again.
* **Replies are bound to their command by name.** A `$R:` / `$R;` reply opens
  with "an exact copy of the command as entered by the user" (p.62). The
  Session compares the echoed command *name* with the outstanding one and
  ignores anything else, so a stray prompt — SsnRx writes `\r\n` on its own
  after 5 s of silence, and the receiver answers it with a bare prompt — can
  no longer shift the whole sequence by one command.
* **Every `set` is immediately followed by its `get` readback.** Fields whose
  echo is deterministic are byte-compared (case-insensitive, quotes stripped);
  fields whose echo is an expanded list (satellite/signal lists, SBF block
  lists, NMEA message lists) or encrypted (the NTRIP password) are read back
  for presence only. The final `lstConfigFile, Current` must additionally
  contain nothing but the driver's own commands. Any mismatch is a startup
  failure.
* **The yaml is the single source of truth.** Every documented key is
  required, and an unknown or misspelled key is a startup error — a typo can
  never leave a compiled-in default in force.
* **Warm-up gate.** Recording starts only when `ReceiverStatus.UpTime` ≥
  `receiver.warmup.min_uptime_s` (default 1200 s: PTP accuracy reaches
  0.1 ms P99 only after 20 minutes, p.47) **and** `RxState` bit 6 FINETIME is
  set (receiver time synchronised to within the `setClockSyncThreshold` limit,
  p.45/p.374; sticky until the next reset). `init()` blocks until then.
* **A failed disk write stops the driver.** `SbfWriter::WriteBlock` reports
  failure, the Session logs it at critical level, emits `FatalError` and the
  rig shuts down. The .sbf file is the record of truth; a write that cannot
  land must never scroll past while the GUI still shows "recording".
* **Recording runs on its own thread.** SsnRx parses the socket on the Qt
  event thread; `Session::OnSbfBlock` only hands the block to
  `SbfWriteQueue` (`include/sbf_write_queue.h`), whose thread feeds the
  synchronous `SbfWriter`. A disk stall therefore never stops the socket
  reader (which would make the receiver drop blocks or close the link). The
  queue is bounded by `output.write_queue_mb` (256 MiB ≈ many minutes at the
  receiver's rate); a block that does not fit is *refused*, never dropped, and
  the run ends (`queue_pending=`/`queue_max=` in the status line show how far
  the disk lags). Every accepted block reaches the disk before `Shutdown()`
  returns.
* **Fail-fast while recording.** Once the gate has opened, a link loss, a
  damaged block (CRC/length), a receiver reset (`UpTime` going backwards) or a
  write failure ends the whole rig at once — the `.sbf` would be incomplete
  either way, and the operator restarts immediately. Reconnect and re-warm-up
  only exist *before* recording, where nothing accepted can be lost; damaged
  blocks in that phase are counted (`crc_errors`, `length_errors`) but only
  touch the prewarm context. `recording_errors` in `drivers.json` is the
  number of fatal events (0 or 1).

## Sequence

`build_command_list()` in `src/commands.cpp`; the Session sends one command,
waits for its reply (`$R:`/`$R!` success, `$R?` error, p.62) and moves on.
`lst` replies open with `$R;` and are complete at the prompt that follows their
formatted blocks, not at the `$R;` line.

| # | Command | Reply handling | Page |
|---|---|---|---|
| 1 | `login, <user>, <password>, RxAdmin, S3pt3ntr10` | `$R?` is fatal. Creates `User1` and copies Current→Boot only on a receiver without any full-control account; otherwise a plain login | 18, 87 |
| 2 | `exeCopyConfigFile, RxDefault, Current` | fatal on `$R?`; runs before every read-only step so the previous session's streams are off | 69 |
| 3 | `login, <user>, <password>, RxAdmin, S3pt3ntr10` | the reset wiped the accounts and the session authorisation, so the same command re-creates the account (`User1`, Current→Boot) and re-authorises the session | 18, 87 |
| 4 | `getReceiverCapabilities` | Aux1 must be listed (dual-antenna attitude is always on) | 74 |
| 5 | `lstAntennaInfo, Overview` (only when a type ≠ `Unknown`) | every configured antenna name must be an `ID="…"` of the XML, exactly (case, spaces); otherwise fatal with the closest names | 66, 107 |
| 6 | `setPPSParameters, …` + get · `setNTPServer` + get · `setPTPServer` + get | the receiver is the rig's time source, so its outputs are restored first after the reset (delay 2 decimals, width 6) | 156, 155, 158 |
| 7 | `setDataInOut, <pin>, , +NMEA` + get · `setNMEAOutput, StreamN, …` + get per `receiver.nmea_streams` | Cd/interval compared; message list presence-checked | 168, 193 |
| 8 | `setDataInOut, <our IPxx>, , +SBF` + `getDataInOut` | an IPxx port opens in the default modes: `auto`, `SBF+NMEA` (p.169) | 168 |
| 9 | `setAntennaType, Main/Aux1, <name>` + get · `setAttitudeOffset, h, p` + get · `setINSAntLeverArm, x, y, z` + get | names verified at step 5, quoted when they contain spaces; the receiver echoes the same fixed-point text the driver sends | 107, 141, 113 |
| 10 | `setCN0Mask, all, N` · `setElevationMask, all, N` · `setSatelliteTracking` · `setSatelliteUsage` · `setSatelliteHealthOverride, v, v` · `setSignalTracking` · `setSignalUsage, v, v`, each with its `get*` | mask values compared on the first echo line; list echoes are expanded, presence-checked (`none` compared exactly) | 94, 110, 96, 128, 127, 100, 131 |
| 11 | `setIMUStartupDataMode` + get · `setIMUOrientation` + get | readback; the thetas are compared when the mode is not `SensorDefault` | 173, 112 |
| 12 | `setGNSSAttitude, MultiAntenna` + get | readback (constant) | 142 |
| 13 | `setPVTMode, Rover, v` · `setReceiverDynamics` · `setVehicleApplication` · `setClockSyncThreshold` · `setMultipathMitigation, b, b` · `setINSNavConfig, on, all, loc`, each with its `get*` | first fields of the echo must match (`PVTMode`: `Rover`; `INSNavConfig`: `on, *, loc`) | 123, 125, 135, 152, 95, 115 |
| 14 | `setNtripSettings, NTR1, …` + get · `setNtripTlsSettings` + get — or `setNtripSettings, NTR1, off` + get | all fields compared except the encrypted password (p.185); the user name is compared | 185, 187 |
| 15 | `lstConfigFile, Current` | every line without `#` must name a command the driver sent (`driver_command_whitelist()`, plus `setUserAccessLevel` — the account line the login left); otherwise fatal | 68 |
| 16 | `setSBFOutput, StreamN, …` + `getSBFOutput, StreamN` per `receiver.sbf_streams` | Cd and interval compared; the block-list echo is the expanded group content, presence-checked | 202 |

Step 15 runs before step 16 on purpose: once SBF streams on the same socket,
a multi-block listing could be interleaved with binary data. The vendored
SsnRx SDK's reply limit was raised to 256 KiB for the listings
(`3rd_party/RxTools/ssnrx/include/ssnrx.h`).

Log lines (module `AsteRx`): `Connecting to …` → `Connected` →
`Configuring receiver on IPxx (N commands)` → `AsteRx Capabilities: …` →
`Antenna types verified …` → one `Verified <Key> (<payload>)` per readback →
`Verified running configuration: M command(s), all ours` →
`Streams enabled — warming up (min uptime 20:00, FINETIME required)` → every
30 s `Receiver warming up: up 07:12 / 20:00, FINETIME not yet set` →
`Acquisition started (receiver up 20:01, FINETIME set)`.

## Warm-up gate

* Source: `ReceiverStatus` (4014, 1 Hz at `OnChange`) — `UpTime` = "Number of
  seconds elapsed since the start-up of the receiver, or since the last reset"
  (p.373); `RxState` bit 6 FINETIME (p.374). The yaml must therefore keep
  `ReceiverStatus` (or the `Status` group) in a stream at an interval other
  than `off`; validation enforces both.
* During the warm-up the streams already flow: the live `live_*.csv` are
  written (the GUI shows the progress), the `.sbf` writer is not fed, the
  watchdog stays armed. `[Statistics] warmup=07:12/20:00  finetime=0 …` is
  printed instead of the recording line.
* `init()` blocks until the gate opens. `main.cpp` initialises the drivers
  sequentially with AsteRx first, so the whole rig's recording start waits for
  the receiver warm-up (up to 20 minutes after a cold start). Ctrl+C aborts.
* A receiver reset before recording normally drops the TCP session: the
  reconnect path reconfigures and warms up again. A drop of `UpTime` while
  recording is fatal (fail-fast, see the rules above).
* `receiver.warmup.min_uptime_s: 0` with `require_finetime: false` disables
  the gate (bench tests).

## Antenna types

`setAntennaType` accepts only names from the receiver's own table (p.107): "If
*Type* does not match any entry in the list returned by lstAntennaInfo,
Overview, the receiver will assume that the phase center variation is zero at
all elevations and frequency bands." The driver therefore refuses to start
when `main_antenna_type`/`aux_antenna_type` is not an exact `ID` of that list
(`Unknown` = no model, always accepted). Look the name up in RxControl
(Expert Console: `lstAntennaInfo, Overview`, p.66) and copy it with its spaces.

## NTRIP client

`setNtripSettings` (p.185): the password is limited to 32 characters and the
characters `" ' $ & ,` are sent as `%%DQ %%SQ %%DL %%AM %%CM` (p.64) by the
driver; other characters must be in the receiver's message charset. The
receiver encrypts only the password, so every other field — including the user
name — is compared against the readback. `<?>` is treated as a placeholder and
rejected when `enabled: true`. The password is redacted from every log line
(`redact_cmd`). The NTR1 connection decodes RTCMv3 in its default `auto` input
mode; no `setDataInOut` is needed.

## Not done, and why

| Command | Reason |
|---|---|
| `factoryReset` (p.86) | erases the accounts and all user settings; keeps IP, but the driver would have no account left |
| `exeResetReceiver, …, Config` (p.79) | reboots the receiver, erases `Current` and `Boot` (accounts included) |
| `exeCopyConfigFile, Current, Boot` (p.69) | volatile configuration by decision; other users' `Boot` stays theirs |
| `lstCurrentUser` (p.84) | a failed login already answers `$R?` (p.87); the extra round trip proves nothing |
| `setINSPOILeverArm` (p.116) | no POI geometry in the yaml; `ins_output_location: POI1` therefore means a zero POI lever arm |
| `setSignalHealthOverride`, `setTimingSystem`, `setSmoothingInterval`, `setDiffCorrUsage` | receiver defaults (`none`, `auto`, no smoothing, `LowLatency`) after the reset |
| any `setIP*`, `setEthernetMode`, `setIPServices`, `setPortFirewall` | network; the platform reaches the receiver only by its IP |
| SBF block-name validation | appendix B lists 113 blocks plus 20 groups; an unknown name is rejected by the receiver with the offending command in the log |

## Account provisioning

`device.user` / `device.password` in `config/config-asterx.yaml` is the
dedicated driver account (level `User`, p.90). It must be 1..16 characters
(`login` UserName (16), Password (32/2), p.87), must not be `RxAdmin`, and may
only use the receiver's command-line charset (p.64) — in particular no `,` or
`"`, which would split the command, and no empty value, which would *delete*
the account (p.17).

Provisioning is never needed: the long login form creates the account when the
receiver has none, and after the step-2 reset that is always the case.

## Side effects of the account re-creation (step 3)

At that moment no full-control account exists, so the receiver executes
`setUserAccessLevel, User1, <user>, <password>, User` **and**
`exeCopyConfigFile, Current, Boot` (p.18): `Boot` becomes "defaults + the
driver account", other users' accounts and saved settings are gone, and the
account lands in `User1` — on every bring-up.

## Recovery

* Anonymous users have full control over COM and USB by default
  (`setDefaultAccessLevel`, p.85) — a serial/USB console can always run
  `setUserAccessLevel`.
* Ethernet-over-USB always answers on `192.168.3.1` (p.15, cannot be changed).
* `login, <user>, <password>, RxAdmin, S3pt3ntr10` over TCP/IP re-creates the
  first account when none exists (p.18).
* `factoryReset` (p.86) as the last resort: keeps `setEthernetMode`,
  `setIPSettings` and `setIPPortSettings`.

## Credentials in logs and recordings

Only the NTRIP password (a remote credential) is redacted from log lines
(`redact_cmd()`); the receiver's own account is local to the sensor and
appears in clear. The receiver's `Commands` SBF block records entered
commands, but they are only ever entered before `setSBFOutput` enables
streams on this connection, so none of them reach the `.sbf` files.

## Open points (first run on hardware)

* **Reply binding.** The Session compares the echoed command *name*, not the
  full text, because a receiver that normalises whitespace would otherwise
  reject every reply. If the log shows `Reply echoes '…' while '…' is
  outstanding`, the receiver is answering out of order — capture the trace log
  before changing the check.
* **`getCN0Mask` / `getElevationMask`.** Both answer with one line per signal
  or engine and only the first is compared, because the GPS P-code mask is
  fixed at 1 dB-Hz by the receiver (p.94) and would fail an all-lines check.
  If the first line turns out not to be the one we set, compare all lines
  except the P-code entry rather than dropping the check.
* `lstAntennaInfo, Overview` larger than 256 KiB would not be captured; the
  driver then fails when a type ≠ `Unknown` is configured.
* Explicit satellite/signal lists naming a constellation the receiver is not
  permitted for may answer `$R?` (fatal by design); `all` is known-good.
* If the post-reset listing shows a line the receiver generates itself (not
  sent by the driver), add its command name to a documented allow-list constant
  rather than relaxing the check.
