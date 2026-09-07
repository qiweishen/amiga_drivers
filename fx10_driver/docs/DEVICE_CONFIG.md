# FX10 camera configuration

On every bring-up (the `AmigaDrivers` session start and the `fx10_snapshot`
tool) the driver returns the camera to its factory
baseline and then applies its own configuration, so that a setting left on the
camera by a previous user can never influence a recording. Nothing is saved on
the camera; the IP configuration is never written. Page numbers refer to the
Specim FX10 user manual (`docs/161584-Specim-FX10-Reference-Manual.pdf`,
User Manual 2.2).

## Rules

* **Baseline = `CameraHeadFactoryReset` then `UserSetLoad`.** The manual
  documents no user sets, no IP configuration and no reset command (it defers
  the GenICam interface to the GigE Vision standard, p.20), so both node names
  need confirmation in the device's GenICam node map. The current driver
  requires both commands and fails bring-up if either is unavailable.
  An acknowledged command and subsequent control readiness do not prove
  every factory value; the driver verifies the configuration it applies.
* **Order.** The reset runs after connect and before the stream open (it resets
  `GevSCPSPacketSize`, which `openStream()` then negotiates or writes), after a
  best-effort `AcquisitionStop` — user sets load only while acquisition is
  stopped.
* **Completion and readiness are separate.** SDK-side `AnswerTimeout` raised to 10 s and
  `DisconnectOnAnyTimeout` cleared for the duration (eBUS Player precedent),
  commands checked for Execute errors. No `IsDone` query is issued: the supplied
  eBUS 6.5.1 `UserSetsManager::Load()` uses Execute followed by parameter refresh,
  and the FX10 log showed one IsDone call taking 10004 ms before link loss.
  This observation does not establish that the query caused the disconnect.
  Each acknowledged command is followed by cache invalidation and positive
  Width/Height/PayloadSize/PixelFormat readback on the existing connection.
  Temporarily unavailable nodes are polled every 200 ms within a 15 s readiness
  budget, with progress logged every 2 s. This wait never re-executes a command.
  There is no disconnect/reconnect inside factory preparation; connection loss
  or expiry of the readiness budget fails bring-up. Blocking SDK calls cannot
  be interrupted and may overrun the budget.
  A failed Execute is not automatically replayed. Both communication parameters
  are restored after each command stage. Readiness does not prove every
  factory value; subsequent required acquisition writes are still verified.
  The snapshot tool and acquisition app share this workflow.
* **Volatile.** `UserSetSave` is never executed.
* **The calibration ROI is never written.** "The ROI values are provided in the
  calibration pack. ROI feature is reserved for this purpose alone. Do not
  change these values." (p.25). The driver has no ROI role and
  `features.raw` must never carry `OffsetX/OffsetY/Width/Height`
  (`tests/test_config.cpp` enforces this for the shipped templates).
* **A clamp is a failure, not a warning.** Every driver-derived write is read
  back, and a value the camera silently changed (`ExposureTime`,
  `AcquisitionFrameRate`, binning, …) fails the bring-up: those values are
  baked into every recorded line, so a session that quietly used a different
  exposure is worse than no session. `features.raw` keeps warn-on-clamp — an
  operator override is not driver intent — but an unknown node name there is
  still fatal, because a silent no-op is what that layer used to do wrong.
* Any failure in this sequence is fatal (`ControlError`, exit code 2).

## Parameters

Exposed to the operator in `config/config-fx10.yaml` (`acquisition:`), factory
values from **Table 8, p.44**:

| Parameter | Factory | Shipped | Why it is exposed |
|---|---|---|---|
| `exposure_ms` | 5 ms | 10 ms | The primary radiometric control. Config is ms, the node is µs |
| `frame_rate_hz` | 50 | 50 | Freerun line rate; under an external trigger it is the EXPECTED pulse rate and is used only to size the buffer pool |
| `spectral_binning` | **2** | **2** | 2/4/8 → 224/112/56 bands (p.46). The factory offset, black-level and bad-pixel calibration was performed at 2, and the optical FWHM is 5.5 nm, so 1× (1.35 nm sampling) oversamples and departs from the calibrated state. 1 is accepted and is required by MROI |
| `spatial_binning` | 1 | 1 | 1/2/4/8 → 1024/512/256/128 samples |
| `pixel_format` | Mono12 | Mono12Packed | Same bit depth, less wire bandwidth; unpacked to uint16 before the recorder sees it |
| `image_enhancement` (AIE) | Enabled | true | See below — it decides whether the data is resampled and which calibration pack applies |
| `status_line` | off | false | It **replaces the last image row** with camera status (p.34), i.e. it destroys a band |
| `mroi` | Disabled | disabled | 1..512 regions, 1×1 binning only (p.9, p.26) |
| `trigger.*` | camera-internal | external | mode / activation / delay / exposure_control / source and selector entries |

**AIE (Automatic Image Enhancement, p.32-33, p.39).** In-camera wavelength
calibration plus smile and keystone correction, applied by **interpolating every
pixel at a fractional address** — the pixels in the file are resampled, not raw.
It is factory-enabled and the calibration pack that applies depends on it (the
pack with `FX` in its name is for AIE on; the manual recommends it, p.44). The
driver therefore writes it **only when the config asks for `false`**, which is
also why a default configuration never needs a node name the manual does not
print. The state and the matching pack are recorded in every `.hdr`.

Deliberately left at the factory value and **not** exposed:

| Feature | Factory | Why it is not touched |
|---|---|---|
| Digital gain | 0 (×1) | ×2/×4/×8 are binary shifts that zero-fill the LSBs and produce missing codes (p.34) |
| Fine gain / digital offset | 1 / — | Part of the factory calibration; the digital offset is unavailable at ×1 anyway (p.34) |
| NUC (offset + bad-pixel) | Enabled | Factory correction; with AIE on the bad-pixel correction happens regardless (p.34) |
| Readout / interleave mode | Enabled | Switching it requires new `BlackLevelOffset` and image-correction settings (p.22) |
| ROI (`OffsetX/Y`, `Width`, `Height`) | calibration pack | p.25; the driver has no ROI role at all |
| Test images (Ramp, LFSR) | off | They REPLACE the sensor data (p.36-38): a diagnostic tool, not a driver setting |
| Burst / software trigger | — | Timing comes from the SensorSync board's hardware pulses (p.30-31) |
| Strobe delay / duration | — | The strobe is the exposure read-back in the timing chain; changing it breaks that chain |
| Mechanical shutter | open | Present (p.47) but the manual prints no GenICam node name — see Open points |

Written but **not** exposed (hidden, driver-internal):

| Write | Why |
|---|---|
| `AcquisitionMode = Continuous` | The only mode this driver supports |
| `EnAcquisitionFrameRate` | `true` in freerun, **`false` under an external trigger** — a constant frame rate left on by a previous freerun session fights the pulse train (p.28) |
| `MROI_Enable = false` | Written even when MROI is off: it survives a power cycle, so "off" must be asserted, not assumed |
| Counter1 `EventSource = MissedTrigger` + `Counter1_Reset` | The missed-trigger counter (p.27) is BOUND and RESET by the driver, so `missed_triggers=` cannot report some other event source a previous user configured. Optional: a camera without those nodes costs the metric, not the recording |
| `DeviceTemperatureSelector` before each temperature read | Two sensors with different limits — processing board 80 °C, FPGA 90 °C, and the camera CANCELS operation above them (p.44). A read without the selector means nothing |
| `LineSelector` before `LineSource` (`features.raw`), when the node exists | `CameraControl` requires an explicit preceding selector write when the camera exposes `LineSelector`. A camera without this node can expose `LineSource` directly; the driver does not invent a `Line1` entry. The physical strobe routing still needs confirmation on the actual device. |

## Sequence

`Fx10DriverApp::bringUpSession_()` (`src/fx10_driver_app.cpp`) and
`tools/snapshot_main.cpp`:

| # | Step | Detail |
|---|---|---|
| 1 | connect | `PvDevice::CreateAndConnect` (id / MAC / IP) |
| 2 | factory preparation | `PrepareFactoryDefaults()`: stop → reset Execute/check → cache refresh/readback → user-set load Execute/check → cache refresh/readback. Uses the existing connection throughout, without IsDone polling. |
| 3 | stream open | `GevSCPSPacketSize` or `NegotiatePacketSize`, rx buffer, `SetStreamDestination` |
| 4 | `ApplyAcquisitionConfig` | walks `BuildApplyPlan()` — see below. Every write is read back |
| 5 | `readGeometry` | `Width`, `Height`, payload size, `PixelFormat` |
| 6 | streaming | buffers → `StreamEnable` → `AcquisitionStart` |

### Receive payload and image layout

`PayloadSize` determines receive-buffer capacity; it need not equal the tightly
packed pixel byte count. `StreamReceiver::CheckFrame()` validates every received
image against its width, height and pixel type, then requires `GetImageSize()`,
`GetEffectiveImageSize()`, `GetAcquiredSize()` and SDK-declared padding to agree.
Chunk data, unexplained trailing bytes and missing-line/overrun flags fail the
recording. Only declared horizontal/vertical padding is omitted from the ENVI
pixels; packed formats are unpacked per row. The first image logs all these
lengths, padding, chunk count and offsets for diagnosis. A larger allocation
alone does not prove padding is present in the received image.

The ordinary acquisition plan does not write Width/Height/OffsetX/OffsetY.
The nominal band-count calculation is not an ROI-restoration command: a
readback differing from that calculation still needs the camera's calibrated
window to be checked. Do not infer calibrated ROI offsets from dimensions alone.

### The write plan

The order is a hardware contract and lives in one pure, SDK-free function,
`BuildApplyPlan()` (`include/apply_plan.h` carries the reasoning with page
numbers, `tests/test_apply_plan.cpp` pins it without a camera). The GenICam
node names are constants in `apply_plan.h` (`fx10::node::*`), verified with
eBUS Player 6.5.1; `features.raw` is the escape hatch for anything else.

1. `spatial_binning`, `spectral_binning` — first: binning changes the readout
   geometry every later value is validated against (p.46).
2. `pixel_format`.
3. MROI — an index register: `MROI_Index` → `MROI_Y` → `MROI_H` per region,
   then `MROI_Enable`. Needs 1×1 binning; with AIE on, add 3 margin rows at the
   start and 3 at the end of every region and discard them offline (p.26).
4. `status_line`.
5. `image_enhancement` — only when it is being turned OFF.
6. `exposure_mode` → `exposure_time` (skipped under pulse-width exposure, where
   the node is unavailable, p.29).
7. `acquisition_mode = Continuous`.
8. Trigger group: `trigger_selector` first, then (external) `trigger_source`,
   `trigger_activation`, `trigger_delay`, `trigger_mode=On`,
   `frame_rate_enable=false`, then the Counter1 binding and reset; or (freerun)
   `trigger_mode=Off`, `frame_rate_enable=true`, `frame_rate` — the only write
   clamped to the node's live range, because the FX10e REJECTS an out-of-range
   rate instead of clamping it.
9. `features.raw`, last, so an operator can override anything above.

## Discovering node names

The manual names features in the Lumo/ASCII interface, not in GenICam, so a few
roles must be filled in from the camera itself:

Inspect an existing GenICam node-map export or the device's feature list in an
already available eBUS Player. The current `fx10_snapshot` tool has no
`--dump-features` option and performs factory preparation and acquisition; it
must not be treated as a read-only node-dump command. Confirm the node types,
access and entry spellings for `features.raw` (e.g. the AIE node),
`DeviceTemperatureSelector`, `Counter1_EventSource`, and `LineSource`.
Check whether `LineSelector` exists before choosing an entry for it.

## Recording policy: time source, fail-fast, durability

* **Time.** The FX10's only path to GPS time is SensorSync (`sensor_trigger`,
  strobe edges against AsteRx PPS/ZDA). With `sensor_trigger.enabled: false`
  the driver warns at bring-up: the lines then carry the camera's raw GVSP tick
  and nothing that can be associated with another sensor offline. Host time is
  never a time source on this platform (the `host_receive_*` columns of the
  line index are diagnostics, kept for format stability).
* **Fail-fast.** The first lost or unrecorded frame ends the whole rig at once:
  the monitor thread polls `StreamReceiver::LossSeen()` (RetrieveBuffer error,
  failed operation result, non-image payload, BlockID anomaly, requeue failure)
  and `EnviRecorder::LossSeen()` (BlockID gap, rejected buffer, wrong geometry)
  every 200 ms, and the camera's missed-trigger counter on the 60 s telemetry
  tick. An incomplete recording is worthless to the platform; the operator
  restarts immediately instead of finding a DEGRADED run at the end. For the
  same reason there is no reconnect loop any more: a link loss (or an unusable
  stream) ends the run — the frames across the gap are gone either way.
* **Durability off the acquisition thread.** `write(2)` only lands in the
  page cache; the periodic `fdatasync` (`output.flush_interval_mb`) waits for
  the disk and used to run on the acquisition thread, where a slow disk could
  exhaust the ~2 s SDK buffer budget and lose frames. It now runs on a helper
  thread inside `EnviRecorder` on dup'd descriptors; flush failures are counted
  there and folded into `write_errors` when the segment finalizes. The
  finalize itself (truncate → fdatasync → rename → `.hdr`) is unchanged.

## Not done, and why

| Feature | Reason |
|---|---|
| `DeviceReset` | reboots the camera and may re-address; the manual documents only a physical power cycle (p.43) |
| `UserSetSave` | volatile by decision |
| `GevPersistentIPAddress` & co., `PvDeviceGEV::SetIPConfiguration` | network; the driver reaches the camera by its id/MAC/IP and never re-addresses it (a foreign-subnet camera fails `StreamReceiver::connect` with the remedy in the message). Re-addressing is the operator's explicit action: `ebus_set_ip` (common/, web GUI Camera Tools → Set IP) sends a FORCEIP and then writes the persistent-IP nodes so the address survives a power cycle. The former `device.force_ip` block is retired (parsed leniently, ignored) |
| `OffsetX/OffsetY/Width/Height` in `features.raw` | calibration ROI (p.25); removed from the shipped templates |

## Open points (first run on hardware)

* **What `CameraHeadFactoryReset` actually resets**, and whether the subsequent
  `UserSetLoad` lands on the factory set — the driver does not write
  `UserSetSelector`, so it loads whichever set the camera currently has
  selected. Compare device node-map exports before and after a bring-up
  performed by the operator; the manual alone does not establish the reset scope.
* **The AIE GenICam node name.** The manual gives only the Lumo name
  (`Camera.Image.AberrationCorrection.Enabled`, p.39), so the config accepts
  only `image_enhancement: true` (the factory value, nothing is written); to turn
  AIE off, first confirm the node and supported values in the actual device's
  node map before configuring a write through `features.raw`.
* **`Counter1_EventSource` and its `MissedTrigger` entry.** The counter is
  documented (p.27), the node names are not. Both writes are optional: a camera
  without them logs a WARN and `missed_triggers=` reads `n/a`.
* **`DeviceTemperatureSelector` entry spellings** (`ProcPCB` / `FPGA`). A wrong
  entry means the corresponding `temp_*` field reads `n/a`, never a wrong
  number.
* **Whether `LineSelector` exists, and the strobe entry if it does.** The current
  acquisition template writes only `LineSource = ExposureActive`.
  `CameraControl` permits that when the device has no `LineSelector`; otherwise
  it stops before the `LineSource` write and requires an explicit preceding
  selector from the actual node map. Every configured enum write is read back.
  Neither the template nor a unit test proves routing to `ISO_STROBE` pin 2.
* Whether a mechanical-shutter node exists (p.47 documents the shutter, not a
  node name); the driver never actuates it.
