# GO-X camera configuration

On every bring-up (the `AmigaDrivers` session and the `jai_snapshot` tool use
the same `CameraSession`) the driver loads the camera's factory user set and
then applies its own configuration, so that a setting saved on the camera by
a previous user can never influence a recording. Nothing is saved on the
camera; the IP configuration is never written. Page numbers refer to the JAI
GO-X series (Pregius S, PGE) user manual (`docs/Manual_Go-X-Series-PregiusS-PGE.pdf`).

## Rules

* **Baseline = `UserSetSelector=Default` + `UserSetLoad`** (p.165). `Default`
  is the read-only factory set ("cannot be overwritten", p.33; `UserSetSave`
  is invalid for it, p.165) and loading it is JAI's documented way to
  "restore the factory default settings" (p.166). The camera has no
  `UserSetDefault` feature (no configurable power-up set).
* **IP settings are separate.** All network features (`GevCurrentIPConfiguration*`,
  `GevPersistentIPAddress/SubnetMask/DefaultGateway`, `GevIPConfigurationStatus`)
  live in `TransportLayerControl` (pp.126–131), which the user-set section
  does not touch. The driver never writes them. A camera on a foreign subnet
  fails the bring-up with a message naming the remedy: the operator-triggered
  `ebus_set_ip` tool (common/, web GUI Camera Tools → Set IP), which sends a
  FORCEIP and then writes `GevPersistentIPAddress/SubnetMask/DefaultGateway` +
  `GevCurrentIPConfigurationPersistentIP` so the address survives a power
  cycle. The former per-camera `device.force_ip` block is retired (still
  parsed leniently, ignored).
* **Acquisition stopped.** "User settings can only be loaded when image
  capture on the camera is stopped" (p.34): the load runs right after connect,
  before `StreamEnable`/`AcquisitionStart`, after a best-effort
  `AcquisitionStop` (a session that died mid-stream leaves the camera acquiring).
* **Order matters.** The load resets `GevSCPSPacketSize` (1476), `GevSCPD`
  (0) and `NetworkThroughputSafetyMargin` (92) and would reset `GevIEEE1588`,
  so it runs before PTP enable and before the stream open (packet-size
  negotiation, `SetStreamDestination`, `GevSCPD`).
* **Waited until complete.** The heartbeat timeout is 3 s (p.125) and the load
  is a flash read: the SDK-side `AnswerTimeout` is raised to 10 s and
  `DisconnectOnAnyTimeout` cleared for the duration (the eBUS Player's own
  `UserSetsManager.cpp` does the same), then `UserSetLoad` is polled with
  `IsDone` (50 ms, 15 s deadline), the GenApi node cache is invalidated
  (`InvalidateCache`) and `UserSetSelector` is read back. Both communication
  parameters are restored on every exit path.
* **Volatile.** `UserSetSave` is never executed; other users' `User1..3` sets
  stay theirs.
* Any failure of the load is fatal (`SdkError` → startup failure).

## Sequence

`CameraSession::bring_up_session_()` (`src/ebus/camera_session.cpp`):

| # | Step | Detail |
|---|---|---|
| 1 | connect | `PvDeviceGEV::Connect(target, PvAccessControl)`; identity read from `DeviceModelName/SerialNumber/VendorName/Version/UserID` |
| 2 | factory defaults | `CameraController::load_factory_defaults()`: probe `UserSetSelector`/`UserSetLoad` (implemented + available, entry named `Default`) → `AcquisitionStop` → `UserSetSelector=Default` → `UserSetLoad` → `IsDone` → `InvalidateCache` → readback. Log: `Factory defaults loaded (UserSetSelector=Default, N ms)` |
| 3 | PTP | `GevIEEE1588=true` + wait for `GevIEEE1588Status` = slave (when `ptp.enabled`) |
| 4 | stream open | rx buffer, `NegotiatePacketSize`/`SetPacketSize`, `SetStreamDestination`, receiver tuning. **No device feature is written here** — their legal ranges depend on the format/ROI writes that follow (p.130) |
| 5 | `ApplyConfig` | walks `BuildApplyPlan()` (see below). Every write is read back; the read-backs become device.json's `applied` block |
| 6 | streaming | buffers → recorder (creates `<cam>/`) → **`device.json`** → `StreamEnable` → `CounterReset` (external only) → `AcquisitionStart` → **first `telemetry.jsonl` row** |

## The apply plan

`BuildApplyPlan()` (`include/apply_plan.h`, `src/apply_plan.cpp`) is a pure
function, so the ordering below is unit-tested without a camera
(`tests/test_apply_plan.cpp`). Only writes that differ from the factory value,
or that carry operator intent and must therefore be verified, are in it:

| # | Feature | When | Why here |
|---|---|---|---|
| 1 | `GevGVSPExtendedIDMode=On` | always | factory Off (p.129); 16-bit BlockIDs wrap every 65536 frames (~6 h at 3 fps) and break the gap accounting. Also an input to the frame rate's and `GevSCPD`'s maxima (p.130, p.141), so it goes first |
| 2 | `SensorDigitizationBits=12` | 12-bit `pixel_format` | factory 10 Bits (p.136). A 12-bit wire format over a 10-bit A/D records 10 bits of data in 12-bit containers ("the image may have gaps in the histogram", p.45/p.168). Never lowered: 8 Bits also changes the sensitivity (p.45) |
| 3 | `Width`, `Height`, `OffsetX`, `OffsetY` | non-zero `roi.*` | size before offsets: the offsets' maxima depend on the size (p.133-134) |
| 4 | `PixelFormat` | configured | factory BayerRG8 (p.137) |
| 4b | `BlemishEnable=0` | `blemish_correction: false` (the default) | "Disable all" (p.156) also switches off the factory black-blemish interpolation (p.82), so the recorded pixels are the sensor's own output. Written only when it differs from the factory value (Enable). After `PixelFormat`, so the Bayer phase and the readout window are settled before the interpolator is switched off |
| 5 | `NetworkThroughputSafetyMargin` | `throughput_safety_margin_pct != 0` | factory 92 (p.128); it caps the frame rate (p.141), so it precedes it |
| 6 | `AcquisitionFrameRate` | freerun + configured | under an external trigger the pulse train sets the timing and the node is not writable (p.36) |
| 7 | `ExposureTime` | configured | **after** the frame rate: the exposure ceiling is the frame period (p.142), so writing it first gets it clamped against the factory 8 fps |
| 8 | `Gain` | configured | `GainSelector` is already AnalogAll after the load and the GO-X has no "All" entry (p.148), so the selector is not written |
| 9 | `TriggerSelector`, `TriggerSource`, `TriggerActivation`, `TriggerMode=On` | external only | selector first, or `TriggerMode=On` arms the factory selector (AcquisitionStart, p.141). freerun writes nothing: the factory `TriggerMode` is already Off |
| 9b | `CounterSelector=0`, `CounterEventSource=1` | external only; **not required** | Counter0 counts the FrameTrigger events the camera *received* (p.115-116, p.160). BlockID gaps only show frames the camera emitted, so `Counter0 − emitted` is the only camera-side evidence of a trigger that never became a frame. The values are the integer enum indices the manual prints, so a camera without counters (or one numbering them differently) warns and loses the `trig=` metric instead of failing the bring-up. `CounterReset` is deliberately **not** in the plan: it runs just before `AcquisitionStart`, because `CounterEventSource=Off` stops a counter without clearing it (p.116), and triggers arriving between the apply and the start would otherwise inflate `missed=` |
| 10 | `features.raw` | as listed | last, so an operator can override anything above |

Read-back is enforced for every write. For items 1-9 a camera-side clamp is a
**bring-up failure**, not a warning: the clamped value is baked into every
recorded frame header, so a silent 150 ms → 114 ms exposure would make a whole
session quietly wrong. `features.raw` (item 10) keeps the softer
warn-on-clamp behaviour, because those values are not driver-derived intent.
Item 9b is the one exception in the other direction (`FeatureWrite::required =
false`): absent → skipped with a TRACE line, refused → WARN, never fatal.

Everything not in the plan (black level, white balance, LUT, gamma, shading,
chunk data, image flip, compression, ...) is at its factory value — and that is
not an assumption: `device.json`'s raw audit reads all of it back and warns
about anything that deviates.

## Rawness guarantees

"Raw" here means: the written pixel values are as close to the sensor's A/D
output as the camera can deliver — highest bit depth, no demosaic, no digital
processing (digital gain, white balance, black-level offset, LUT/gamma, colour
matrix, enhancers, shading, defect-pixel interpolation), no binning, scaling,
decimation or compression, no flip. Analog gain and exposure are operator
knobs, not processing.

Of every feature in the manual that can change a pixel value or the sensor
readout, exactly **three** have a rawest value that differs from the factory
one:

| Feature | Factory | Rawest | Stage |
|---|---|---|---|
| `SensorDigitizationBits` | 10 Bits | 12 Bits (p.45/p.136) | A/D |
| `PixelFormat` | BayerRG8 | BayerRG12Packed — same values as BayerRG12 in 25 % fewer bytes (p.137/p.168) | output packing |
| `BlemishEnable` | Enable | Disable all (p.82/p.156/p.172) | digital post-processing |

Everything else is **factory = rawest**, verified per feature: `Gain[AnalogAll]`
1.0, `Gain[DigitalRed/Blue]` 1.0, `BlackLevel[All/Red/Blue]` 0,
`GainAuto`/`ExposureAuto`/`BalanceWhiteAuto` Off, `LUTMode` Off (p.172: "LUT
OFF: gamma = 1.0", which makes the `Gamma` register's 0.45 inert),
`ColorTransformationRGBMode` Off, the enhancers Off, `ShadingMode` Off,
`ReverseX/Y` Off (a flip changes the Bayer phase, p.44), `TestPattern` and
overlay Off, `ImageCompressionMode` Off (forced Off under a 12-bit format
anyway, p.92), `GradationCompressionMode` Off with its knee/gain curve at
50 %/0 dB/100 %/0 dB, `ImageScalingMode` Off, decimation 1, binning 1
(mono-only anyway, p.85/p.134), `MultiRoiMode`/`SequencerMode`/`ROICentered`
Off, `ExposureMode` Timed, `ExposureModeOption` Off, `AcquisitionMode`
Continuous.

Why the factory load is needed at all: the manual is explicit that the Working
Set (RAM) is cleared at power-off (p.32) and `UserSetControl` has only
Selector/Load/Save (p.165) — there is **no** power-on user set. The risk is not
that someone's `User1` loads itself; it is that another process, in this same
power cycle, left a register dirty and it is still in force when this driver
connects. `UserSetLoad[Default]` is what pulls all of it back.

`VideoProcessBypassMode` is deliberately **not** written, even though it sounds
like the raw switch:

1. Everything it bypasses (p.47: `Gain[DigitalRed][DigitalBlue]`, `BlackLevel`,
   `BalanceWhiteAuto`, `LUTMode`, `ColorTransformationControl`, `EdgeEnhancer`,
   `ColorEnhancer`, `ShadingControl`, the sequencer's digital gains/LUT) is
   already the identity after the factory load — and colour-space conversion
   and the colour enhancer are only available for RGB formats (p.109/p.110), so
   under BayerRG12Packed they are structurally unreachable, not merely off.
2. The manual never states what happens to the documented `8LSB@8bit` pedestal
   on the bypassed path, and `BlackLevel` is itself in that list.
3. p.47 says those *functions* are bypassed; whether their registers stay
   readable is undocumented — so the behaviour of the read-back audit under
   bypass is itself unknown.
4. The recommendation to bypass (p.100) is written for Gradation Compression
   and auto white balance, neither of which is in use.
5. It does not cover `BlemishEnable`, gradation compression, test patterns,
   overlay, scaling, decimation or compression, so it would not replace the
   audit anyway.

It stays reachable through `features.raw`, and the dark-frame comparison that
would settle it is in "Open points" below.

One thing the driver cannot fix: the black level needs about the first 10
frames after `AcquisitionStart` to stabilise (p.36). Post-processing that cares
about the pedestal should discard them.

## Dataset metadata

Two sidecar files per camera make a recording self-describing. Neither is ever
fatal: a failure to write them warns and the capture continues.

`<cam>/device.json` (once per session, written after the recorder created the
directory and before `AcquisitionStart`):

| Block | What it answers |
|---|---|
| `factory_load` | which `UserSetSelector` entry was loaded, how long it took, whether `IsDone` was supported, and what the selector read back |
| `applied[]` | every plan write in order: `requested`, `readback`, `strict`, `tolerance_rel`. The tolerance is what makes a legal 1 % float quantisation distinguishable from a clamp |
| `raw_audit` | ~45 features the driver never writes plus 4 selector-scoped ones, each with the factory value the manual documents and an outcome of `match` / `deviates` / `recorded` (nothing in the manual to judge it against, e.g. the operator's own `PixelFormat`) / `unreadable`. Only `deviates` warns; `unreadable` is the normal reading for the mono-only and RGB-only features |
| `identity` | vendor/model/serial, **`DeviceVersion` and `DeviceFirmwareVersion` separately** (p.124-125), FPGA version, SFNC version, MAC/IP (p.127), `SensorWidth/Height` and `WidthMax/HeightMax` (p.131-132) next to the streamed `Width/Height`, and `BlemishCompensationNumber` |
| `derived` | `exposure_time_us` read back, `exposure_offset_us` = 2.45 (p.38/p.171 — note p.164's chunk formula prints 2 µs), their sum, the black-level strings verbatim from p.172 with the 12-bit figure flagged `derived_by_driver`, and the thermal limit |
| `transport` | `PayloadSize`, `GevSCPSPacketSize`, `GevSCPD`, the throughput margin and the frame rate, with the frame rate's node min/max |
| `runtime` | what "auto" resolved to: buffer count, queue frames, socket RX requested vs effective, negotiated packet size, whether Counter0 bound |
| `ptp` | enable result, lock time, last status/accuracy, and a `timescale` note: the grandmaster's timescale is a rig property the driver does not verify (p.121 only documents a 1 ns count with a 1970 origin), and no host clock is consulted to guess it |

`<cam>/telemetry.jsonl`, one JSON object per device-poll tick (5 s) plus a
first and a last row: `hrt` (host CLOCK_REALTIME ns, the same key `idx.jsonl`
uses), the three `DeviceTemperature` readings (p.125), `trig`/`trig_overflow`
(Counter0), `pause_rx` (`aPAUSEMACCtrlFramesReceived`, p.130) and the PTP
status/accuracy the guard read on the same tick. Unavailable values are
`null`; the row shape never changes. p.173 caution: the internal temperature
must stay below 72 °C, and about 30 min of warm-up is needed for the specified
performance — an excursion warns once (re-armed at 67 °C) and never stops the
capture.

`[Statistics]` lines gain `temp=` (sensor die, when polled) and `trig=`
(Counter0, external trigger only; `(ovf)` when the 32-bit counter wrapped); the
Final line gains `triggers=` and `missed=`. All are appended after the existing
keys, so `app/services/driver_stats.py` and `tools/check_contracts.py` are
unaffected.

Together the two files cost ~5 KB plus ~200 B / 5 s (≈3.5 MB/day). They are
counted by the GUI's directory-size poll, so an idle external-trigger session
now reports a write rate of ~40 B/s instead of exactly zero.

## PTP

The GO-X is a slave-only PTP node with exactly one feature pair,
`GevIEEE1588` / `GevIEEE1588Status` (p.121, p.128) — there is no SFNC
`PtpEnable`/`PtpStatus`, no servo status and no `PtpOffsetFromMaster`, so the
driver targets those two names directly. Time synchronization is performed but
**no frequency tuning**, and the timestamp tick frequency is fixed at 1 GHz
(p.121).

PTP is the camera's **only** absolute time: no host time is used as a time
source anywhere on this platform (the host clock is not trusted), so the
shipped configuration enables it and `ptp.enabled: false` is a warned
configuration — the frames then carry free-running ticks that cannot be
associated with anything offline. The grandmaster is the AsteRx RBi3 Pro+
(its own address, GPS timescale per the rig configuration); the driver records
that as an unverified note in `device.json` rather than measuring it against
the host clock (the former host/device cross-check and its 37 s TAI
assumption are gone). Prerequisite, not verified on hardware: the grandmaster
must share the camera's L2 domain — PTP multicast does not cross subnets, and
with one host NIC per device subnet that means a common switch or host-side
bridging.

While synchronized, the capture loop calls `PtpManager::check_health()` on the
**5 s device-poll tick** (`kDevicePollIntervalS` in `src/capture_runner.cpp`,
shared with the telemetry row, guard first so the row carries what the guard
just read): `GevIEEE1588Status` must still read `slave` and
`GevIEEE1588ClockAccuracy` must stay within **0..9** — that window is the
driver's choice, not the register's range: p.128 gives it as 0..20 with a
factory value of 19 (Unknown), so an un-synced camera reads outside the window
by construction (Within25ns … Within1ms are 0..9; 10-18 are 2.5 ms or worse,
20 is Reserved). A failing reading is
re-checked three times one second apart before the session is stopped, so a
poll that lands mid-BMCA does not end a recording. If the accuracy register is
not readable at all the guard says so once and continues on the status alone.

`GevIEEE1588ClockAccuracy` is an *enumeration*, which the eBUS
`GetIntegerValue` helper cannot read (it rejects a node of the wrong type), so
the read goes integer → enum entry value (`PvGenEnum::GetValue`, the number the
manual prints) → entry name mapped back through
`ptp_clock_accuracy_from_name()`. Getting that order wrong is not a loud
failure: the accuracy half of the guard simply never runs and every recorded
`ptp.accuracy` is `null`.

> First run on hardware: `GevIEEE1588ClockAccuracy` is defined by GigE Vision
> as the accuracy a device *advertises* as a potential grandmaster, and the
> GO-X ships at 19 (Unknown). If this firmware never updates the register while
> slaved, the guard will trip right after the first sync — in that case either
> the threshold or the guard's scope needs revisiting against the observed
> values.

## Fail-fast and on-disk residue

* Every counter that would make the session's final verdict "not clean"
  (queue overflow, BlockID gap, incomplete frame, buffer error, stream-layer
  block drop) is checked on the 200 ms monitor tick, and the first non-zero
  one stops the whole rig at once. An incomplete recording is worthless to the
  platform; the operator restarts immediately instead of discovering the loss
  at the end.
* `Recorder` preallocates each segment (`fallocate`, 2 GiB, `KEEP_SIZE`) and
  releases the unused tail with `ftruncate` when the segment closes. After a
  crash (kill -9, power cut) the last segment of every camera can keep up to
  2 GiB of preallocated-but-unused blocks: `ls -l` shows the written size while
  `du` shows the allocation. The data is intact; the space is reclaimed when
  the file is rewritten or removed.

## Not done, and why

| Feature | Reason |
|---|---|
| `DeviceReset` (p.125) | reboots the camera; with the default DHCP/LLA configuration it may re-address and needs re-enumeration |
| `UserSetSave` | volatile by decision; `Default` cannot be saved anyway |
| `GevPersistentIPAddress` & co., `PvDeviceGEV::SetIPConfiguration` | network; the driver reaches the camera by its IP and never re-addresses it. Re-addressing is the operator's explicit action (`ebus_set_ip`, GUI Camera Tools → Set IP) |
| `UserSetDefault` | not implemented by the GO-X |
| `VideoProcessBypassMode=On` (p.47) | five reasons, in "Rawness guarantees" above. Short version: the chain is already the identity, the manual does not say what bypass does to the documented pedestal, and it would make the read-back audit's own behaviour undefined. Available through `features.raw`, and a dark-frame comparison is in "Open points" |
| Explicitly writing the ~45 factory values the raw audit checks | every write is a new way to fail on an enum spelling this firmware happens to disagree with. The factory load already establishes them; the audit both confirms it and leaves the evidence in `device.json` |
| `ChunkModeActive` per-frame metadata (p.164) | of the nine chunk data fields only `ChunkLineStatusAll` and `ChunkFrameTriggerCounter` say anything the driver does not already know — there is no timestamp and no frame ID in the chunk table — and adding a chunk tail means a jai-raw-seg v1.1 with four readers (`inspect_raw.py`, `unpack_raw.py`, `live_view.py`, `gen_test_segment.py`) to migrate |
| GVCP event channel (p.162-163) | every `Event*FrameID` reads "0 (Fixed)", so events cannot be matched to frames; the event state is cleared once the message is sent (p.119) and the message channel would have to be configured (p.129) |
| `BlemishDetect` / dumping the blemish table at bring-up | detection needs a covered lens, the full ROI and Continuous mode (p.81). The driver only reads `BlemishCompensationNumber` |
| `TimestampReset` via an Action command | disabled while PTP is synchronized (p.121) |
| `DeviceLinkHeartbeatTimeout` raise (p.125) | the factory load restores 3 s on every bring-up, but the heartbeat is sent by an SDK thread that a disk stall does not block. Left factory until a link loss is actually observed |
| `ChunkModeActive` (p.164) | enlarges every payload (p.126) and needs acquisition stopped to change (p.120); 64-bit BlockIDs plus host timestamps already give complete frame accounting |
| `BalanceWhiteAuto` (p.149) | white balance is digital post-processing and its auto range is only 3000-9000 K (p.172); raw Bayer is balanced offline instead |
| `ImageCompressionMode` (p.92) | Lossless requires an 8-bit format, and the host-side decompression library implements 8-bit only (p.96) |

## Open points (first run on hardware)

* The load resets `DeviceLinkHeartbeatTimeout` to 3 s; if the SDK's
  `DefaultHeartbeatTimeout` is larger the device value may have to be
  rewritten after the load. Watch for link-loss right after the load.
* `IsDone` behaviour on this firmware (a `GENERIC_ERROR` is handled: the
  command is taken as complete once `Execute` returned).
* Whether `DeviceUserID` survives the load is not documented; the driver does
  not depend on it (connect by MAC/IP).
* `GevIEEE1588ClockAccuracy` while slaved (see the PTP section above).
* `TriggerSource` is written as the manual's integer enum value (24 = Line5
  Opt In) because the symbolic entry name is not printed anywhere in the
  manual. A symbolic name can be configured instead if the camera's own
  spelling turns out to be more convenient.
* **Does bypass change anything?** Cover the lens, add
  `{name: VideoProcessBypassMode, value: "On"}` to `features.raw` in
  `config-gox-snapshot.yaml`, take five GUI snapshots with and without it, and
  compare `mean_16` and the histogram's lower edge against the driver's derived
  ≈128 LSB @ 12 bit pedestal. Also check whether the features in p.47's list
  turn `unreadable` in `device.json`'s audit while bypassed. Record the outcome
  here; until then bypass stays unwritten.
* **`BlemishCompensationNumber` on a virgin camera.** Non-zero means the
  factory blemish table is enumerable through
  `BlemishCompensationIndex/PositionX/Y` (p.157) and post-processing can
  reproduce the interpolation itself; zero means switching correction off
  discards information the camera will not hand out. The manual does not say
  which it is — `device.json`'s `identity` block answers it on the first run.
* **Does Counter0 count while acquisition is stopped?** `CounterReset` runs
  immediately before `AcquisitionStart` precisely because the manual does not
  say. If `missed=` comes out systematically negative or inflated, this is the
  first thing to check.
* **`DeviceTemperatureSelector` while streaming.** It is a DeviceControl
  selector for a read-only float, so it should not be affected by
  TLParamsLocked, and the telemetry poll writes it every 5 s. A firmware that
  refuses it degrades to `null` with a single warning — watch for that warning.
* **Audit spellings.** The expectation table compares against what `ToString()`
  returns, which varies by node type (`Off` vs `false` vs `0`); the table
  carries alternatives where the type is not obvious. A `deviates` warning for
  a feature that is actually at its factory value means the table needs the
  camera's spelling added, not that the camera is wrong.
* The manual prints the exposure offset as **2.45 µs** (p.38, p.171) but the
  `ChunkExposureTime` formula on p.164 says 2 µs. `device.json` records 2.45
  and this discrepancy; a measurement would settle it.
