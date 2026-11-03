# FX10 camera configuration

On every bring-up (`AmigaDrivers`, `fx10_snapshot`, or the start of an
`fx10_reference` pair) the driver executes factory preparation and then applies its own
configuration. The scope of the reset and unverified settings remains unknown;
this does not establish that every setting left by a previous user is cleared.
Before each normal acquisition, it issues the mechanical-shutter open pulse and
checks the SDK write result. The pulse value does not report shutter position or
motion completion. Nothing is saved on
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
print. The state and the matching pack are recorded in device/profile metadata and offline-export provenance.

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

The mechanical shutter is **not** left at an assumed default. Table 8 on p.44
says it opens at power-up; that is not a guarantee for a later acquisition,
configuration reset or reconnect. See the mandatory shutter action below.

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

The shared [eBUS lifecycle](../../ebus_driver/README.md) owns connect, SDK
buffers, workers and RAW storage. The FX10 profile loads factory defaults,
applies the ordered write plan, reads actual image geometry and resolves the
calibration axis. It opens the shutter (or requests closed for a dark reference)
before AcquisitionStart, then establishes the missed-trigger counter baseline.
Device-specific shutter commands remain in CameraControl; no tool implements
a second acquisition loop.

### Mandatory mechanical-shutter action

The operator supplied the following FX10e control definition. The supplied FX10
User Manual 2.2 does not document these node names; this mapping is based on the
operator's device information, not a claim of confirmation from that manual.

| Node | Numeric range | Operation |
|---|---|---|
| `MotorShutter_PulseRev` | 1..255 | Writing 255 opens the shutter. |
| `MotorShutter_PulseFwd` | 1..255 | Writing 255 closes the shutter. |

**The write causes the action; the stored value is not shutter state.** Both
entry points call `OpenShutter` after all acquisition configuration writes and
before `StreamReceiver::Start`. It invalidates the node cache, obtains the
numeric node as `PvGenInteger`, checks writability and that its reported range
contains 255, then calls `SetValue(255)` exactly once. It never skips the write
because a previous value was 255, calls `PvGenCommand::Execute`, polls a stored
value as position, or uses the generic `SetInt` readback to claim motion completion.
The driver has no configurable shutter binding or enable/disable switch; the
normal acquisition and snapshot YAML files need no `shutter` section.

Missing/wrong-type/unwritable nodes, range-read errors, an unsupported pulse,
SDK write failure (including timeout), cancellation or disconnect stop startup.
A failed or ambiguous write is not automatically repeated. Cancellation is
checked before and after the blocking SDK operation. A successful SDK result
permits startup to continue, but does not confirm mechanical motion completion.
No motion-complete signal or settling time has been provided, so the driver
does not invent a state check or a fixed wait.

`device.json` records the requested action in `profile.shutter_request`; it does
not contain an unexecuted `shutter_preparation` placeholder. After the startup
hook succeeds, `events.jsonl` records a `profile_start` event whose
`actions.shutter_preparation` contains the actual pulse node/value,
device-reported range, SDK acknowledgement, elapsed time, acknowledged open/close
status and `state_verified: false`. This result comes from the pulse operation's
cached outcome, without another device-configuration readback. If the startup
hook fails, the existing error/cleanup path runs without publishing
`profile_start`; this event does not itself prove AcquisitionStart succeeded.
`features.raw` rejects `AcquisitionStart`, `AcquisitionStop` and both motor-shutter
pulse nodes to preserve the order and prevent duplicate/opposing motor writes.
The normal startup does not write `MotorShutter_PulseFwd`; the separate
reference tool uses it for the dark phase described below. External trigger start still
follows `AcquisitionStart`; this does not establish that an independently
controlled trigger source was idle beforehand.

Future operator verification should cover repeated acquisitions with the stored
value already at 255, initially open/closed shutters, missing or wrongly typed
nodes, incompatible range, rejected/timed-out writes, cancellation and both
entry points. Check the actual motor response and whether the first retained
exposure needs a documented settling interval. Config tests, C++ execution and
hardware verification were not run for this change.

### Collect Reference page

The UI invokes `fx10_reference` using the saved FX10 YAML and output root.
The operator places an illuminated white reference before starting. The tool
captures white/open and dark/closed phases, each for reference.duration_s.
The config text is read once and snapshotted.

Both phases now use independent EbusCameraDriver instances and connections.
Before dark acquisition, serial/model, geometry, PixelFormat, exposure/trigger
readbacks, ordered applied entries and calibration references must match.
The manifest explicitly says `connection_scope="separate verified phases"`;
undocumented factory state, AIE implementation and mechanical shutter position
are not proven identical. The shutter remains closed after the dark phase until
a subsequent normal/snapshot run requests opening it.

Each phase records device.json, seg_*.raw and events.jsonl, plus
reference_phase.json. A SensorSync participant has its own phase timing log.
The root stores raw/config/config-fx10.yaml, reference.request.json,
reference.json and reference.log. An interrupted or failed phase preserves its
partial recording and publishes no successful pair.

CLI markers CONFIG/SESSION/PHASE/OK/FAIL and the GUI workflow remain supported.
Plots decode supported RAW image buffers offline in DN, exclude invalid records,
and retain the RAW data if rendering fails. Old ENVI reference sessions remain
readable. See [format and lifecycle details](../../ebus_driver/README.md).

### Receive payload and image layout

The acquisition path copies the entire eBUS acquired buffer unchanged. It does
not unpack Mono10/12, strip padding or Chunk bytes, delete a status line,
convert to uint16, or assemble an ENVI cube. Every record carries the SDK's
PixelFormat, geometry, image offset, padding, image/effective sizes and status.
An SDK image represents one pushbroom scan line with spatial samples across
width and spectral rows across height; a sequence forms a scan over time.

Known image formats are decoded by the shared Python reader for preview and
explicit offline ENVI export. Unsupported PFNC is not a reason to transform or
discard safe payload bytes. A decoder rejects unknown layouts rather than guessing.
Actual ROI, MROI, binning and device-side processing still determine what the
camera has already output.

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

All capture paths now use the [shared RAW recorder](../../ebus_driver/README.md).
It stores the raw SDK timestamp integer with unverified unit/epoch and the
host reception realtime/monotonic observations separately. SensorSync
association is evaluated from its observation log, not inferred from a common
host timestamp. Shared-trigger participants register during Init and arm in Run.

output.max_frames counts admitted valid image buffers. max_duration_s starts
at the AcquisitionStart command. rotation.max_lines now limits SDK records per
segment (including flagged/non-image buffers); max_mb limits total RAW bytes.
on_gap remains accepted as legacy configuration, but RAW never inserts zeros:
gaps are metadata and trigger a failed session. Existing config validation and
device apply semantics remain in the FX10 profile.

The shared engine records safe flagged buffers and fails on loss, queue
exhaustion, SDK or file errors. It drains admitted buffers, synchronizes segments
and records session_end only during finalization. A missing/failed final result
is incomplete. Native RAW remains authoritative after an offline ENVI export;
the exporter publishes a derivative header only after its data/index are complete.

## Dataset metadata

device.json contains ebus-device-v1 identity, factory actions, applied writes,
final configuration readbacks, calibration/profile, SDK header version and the
actual buffer budget. Runtime SDK version is unknown unless queried.
The profile records the actual geometry and expected frame rate separately;
configuration is not evidence of an achieved rate.

The `profile_start` event contains only startup action results in `actions`:
`shutter_preparation` and `missed_triggers` with `baseline` (null when unreadable)
and `binding_verified`. It does not repeat the static `device.json` snapshot or
reread its configuration nodes. Start/periodic/stop `device_health` events preserve
temperatures, raw missed-trigger counter, baseline, delta, binding and regression
evidence. `stream_final_statistics` records the
available SDK counters. sensor_sync_quality and capture_final retain final
accounting; session_end distinguishes SDK records from valid frames.

Each RAW record independently describes SDK byte count, image offset and layout,
padding, PixelFormat, Chunk count, operation status, ID and timestamps.
A counter unavailable to the profile remains null; unknown metadata cannot be
promoted to a synchronization claim. The historical capture.json, .hdr, .lines.csv,
segments.jsonl and telemetry.jsonl belong to the legacy ENVI format, which the
reader still supports.

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
  acquisition template writes `LineSelector = LineOut0`, then
  `LineSource = ExposureActive`. `CameraControl` also permits a direct source
  write on a device without `LineSelector`; when a selector exists it requires
  an explicit preceding selector write. Every configured enum write is read back.
  Neither the template nor a unit test proves routing to `ISO_STROBE` pin 2.
* **Mechanical-shutter completion timing.** The operator supplied the numeric
  `MotorShutter_PulseRev` / `MotorShutter_PulseFwd` actions (255 opens/closes),
  but their stored values cannot confirm position. Actual motion-completion
  feedback and any required settling interval remain unspecified; the driver
  records only acknowledgement of the explicit open/close writes.
