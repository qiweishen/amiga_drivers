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

`Fx10DriverApp::BringUpSession()` / `StartStreaming()` (`src/fx10_driver_app.cpp`) and
`tools/snapshot_main.cpp`:

| # | Step | Detail |
|---|---|---|
| 1 | connect | `PvDevice::CreateAndConnect` (id / MAC / IP) |
| 2 | factory preparation | `PrepareFactoryDefaults()`: stop → reset Execute/check → cache refresh/readback → user-set load Execute/check → cache refresh/readback. Uses the existing connection throughout, without IsDone polling. |
| 3 | stream open | `GevSCPSPacketSize` or `NegotiatePacketSize`, rx buffer, `SetStreamDestination` |
| 4 | `ApplyAcquisitionConfig` | walks `BuildApplyPlan()` — see below. Every write is read back |
| 5 | `readGeometry` | `Width`, `Height`, payload size, `PixelFormat` |
| 6 | mechanical shutter | shared `CameraControl::OpenShutter`: write `MotorShutter_PulseRev = 255` once → check SDK write result; failure prevents streaming |
| 7 | streaming | buffers → `StreamEnable` → `AcquisitionStart` |
| 8 | external trigger | `SensorTriggerLog::Start` requests pulses only after the camera starts; the snapshot is freerun and does not request pulses |

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

On successful preparation, production `device.json` records `shutter_preparation`
with the open node/value, device-reported range, SDK acknowledgement, elapsed time,
`status: open-pulse-acknowledged` and `state_verified: false`. Failures use the
existing error/cleanup path before that snapshot is published.
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

The navigation's **Collect Reference** page (`/reference`) is dedicated to FX10;
Camera Tools links to it. Place an illuminated white
reference target in the field of view before clicking. The tool loads the camera
address and acquisition settings from the **saved FX10 config** referenced by
the main config. Use **Apply to config** first when preview slider changes should
be used; table selection and unsaved sliders do not override reference settings.
Exposure, binning, pixel format, geometry/calibration, network settings and trigger
mode/rate follow that config. The collection takes its output root from the main
config's **Output Directory**, as normal rig recordings do.

Set the duration in the saved FX10 YAML:

```yaml
reference:
  duration_s: 5.0
```

`reference.duration_s` is the duration **of each phase**, defaulting to 5 seconds
when omitted. It accepts finite values from 0.1 to 3600 seconds, including
fractions; zero, negatives, NaN, infinity and out-of-range values are rejected
before device connection. Normal recording and snapshot durations are unaffected.
The page shows the saved setting and provides a link to the config editor.
Use **Reload config** after editing; collection always loads the saved file again.

`fx10_reference` performs factory preparation and applies the configuration once,
then uses the same connection for both phases:

1. Write `MotorShutter_PulseRev = 255`, check the SDK result, start the receiver,
   start configured SensorSync logging/pulses, and collect white reference for `reference.duration_s`.
2. Require acknowledgement of `AcquisitionStop`, then drain/finalize all white
   data before moving the shutter. Teardown shares the once-per-run stop flag
   and does not replay this command.
3. Write `MotorShutter_PulseFwd = 255`, check the SDK result, and collect dark
   reference for `reference.duration_s` with the same acquisition settings.
4. Stop/drain/finalize dark data and disconnect. The shutter remains closed;
   the next normal acquisition explicitly issues its opening pulse.

Each duration is a **host steady-clock window after stream/trigger startup**,
not an inferred `frame_rate_hz * duration_s` frame count. Setup and durable finalization
add to the total button-operation time. Startup and in-flight frames are retained
with their original per-line timing observations, so the saved data do not assert
an exact physical exposure span equal to the requested duration. Shutter write acknowledgement still
does not verify position or settling; no undocumented wait is assumed.

For external mode with SensorSync enabled, the configured channel's PWM pair
is enabled: channels 0/1 share FX and channels 2/3 share JAI. Both outputs of that
pair pulse together; the other pair is explicitly disabled for this operation.
Pulses start after `AcquisitionStart`
and stop before disarming the camera. In freerun all SensorSync trigger outputs
are disabled while its timing log records observations. With SensorSync disabled,
external mode needs the independently supplied trigger configured by the operator;
no received frames makes the phase fail.

Persistent output uses a `reference_` prefix on the pair's session directory:

```text
<Output Directory>/reference_<UTC>/
  raw/config/config-fx10.yaml          # exact configuration text loaded once
  raw/fx10/white/fx10_<UTC>/            # ordinary FX10 ENVI session
  raw/fx10/dark/fx10_<UTC>/             # separate ordinary FX10 ENVI session
  reference.log
  reference.request.json
  reference.json                      # completed only after BOTH phases finalize
```

Each phase reuses `EnviRecorder`: `segment_*.bil`, `.hdr`, `.lines.csv`,
`capture.json`, `segments.jsonl`, configured rotation/gap/flush policies, plus
`device.json`, start/stop `telemetry.jsonl`, `stream_stats.txt`, and
`sensor_trigger.log` when enabled. `reference_phase.json` records the phase,
timing, frame count and result. The per-phase stop policy overrides only the
normal recording duration/frame limits with `reference.duration_s` and unlimited frames; it does not
modify the saved source config. These directories are never included in preview
temporary-file cleanup. Existing timestamp collisions receive the usual suffix.

The page has separate **White reference** and **Dark reference** line charts.
Both use the same reductions as Snapshot and Data Live: per band, calculate mean,
median, min, max and p90 over all genuine, non-anomalous frames and spatial samples,
then multiply by `100 / full_scale` from that phase's immutable `capture.json`.
Synthetic padding is excluded using the versioned line index. The reference
charts cover all finalized segments in the whole phase, not the last-second
window used by Data Live. Integer histograms accumulate bounded batches; median
and p90 use linear interpolation at rank `(N - 1) * q`, matching the existing
NumPy reductions without concatenating the full recording in memory. The reader
checks geometry, header layout, file sizes and the phase's written-frame count.

Calibrated captures use their saved wavelength axis; otherwise the horizontal
axis is explicitly **Image row (uncalibrated)**. Each plot shows the requested
duration, actual frame count, geometry, mean and clipping percentage. Plot errors
leave the successful raw recording intact and are shown separately. Completed
results remain available when navigating away and back within the same GUI process;
the next collection clears both previous plots before displaying its new pair.

The GUI reserves camera tools for the whole pair and prevents overlap with main
recording, including a run with FX10 disabled because SensorSync may be shared.
Leaving the page does not release the reservation early. Missing tools/config,
shutter failures, transport/writer loss, verified missed triggers, trigger-log
errors or empty phases do not yield a successful pair; partial data are retained.
The GUI derives the actual duration from the collector's `REFERENCE: CONFIG`
announcement, from the same configuration text saved with the pair. Its collection
timeout is `2 * duration_s + 180` seconds, allowing initialization and finalization;
the later plot calculation is separate. On timeout it requests shutdown. A process
whose shutdown cannot be verified locks further
control operations until its ownership is resolved. The main session `Guards`
configuration does not apply to this standalone tool.

Suggested verification (not executed here): check white/dark frame content and
shutter writes on hardware; repeat with stored pulse values already equal to 255;
compare actual exposure/binning/format with the saved config; exercise freerun
and external triggering, zero-frame input, write failures, repeated clicks,
page disconnect, cancellation/timeout and directory collisions. Confirm both
segments finalize separately and failures never report a completed pair.
Also check non-default/fractional durations, invalid duration rejection, timeout
growth with duration, and post-navigation plot recovery. Compare histogram
reductions against Snapshot/Data Live on the same integer cube (including odd/even
pixel counts, ties, saturation and above-full-scale container values), split that
cube across multiple segments, and verify that padding/anomalous index rows are
excluded and corrupted metadata/indexes produce plot errors. These comparisons
and runtime/UI/hardware checks were not executed here.

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
* **Bounded recording pipeline.** The receive thread validates and copies SDK
  image bytes, then requeues the `PvBuffer`. One recording worker unpacks,
  removes declared padding and writes ENVI BIL, including index events and
  rotation. A frame and its preceding gap share one queue entry. Rejected
  buffer metadata uses the same ordered queue. Queue exhaustion stops the run;
  accepted entries drain before `recorder.Stop()` and before sink destruction.
  `network.buffer_count: auto` now selects 16 SDK receive buffers (subject to
  the SDK limit). `stall_budget_s` sizes the application queue at the configured
  line rate. `max_buffer_memory_mb` caps SDK payload buffers, application payload
  buffers and one canonical scratch frame together; bookkeeping and other
  application allocations are outside that cap. The actual allocations and
  conservative queue-only stall estimate are stored in `device.json`.
* **Durability.** Periodic `fdatasync` (`output.flush_interval_mb`) runs on a
  helper using dup'd descriptors, with at most one active and one pending
  request. Pending requests for the same segment are coalesced. A failed flush
  immediately makes `Failed()` true and stops the session; finalization still
  retries to preserve complete data/index records and accounts for the error.
  The ENVI layout and finalize order (truncate → fdatasync → rename → `.hdr`)
  are unchanged. The cadence is not a promise of loss limited to a fixed time.

## Dataset metadata

The main `Fx10DriverApp` recording path writes the following files in each
connection-epoch directory. The ENVI pixel layout is unchanged. Snapshot tools
also use the recorder's v2 line index, capture contract and segment summaries;
the device/telemetry/stream sidecars belong to the main capture session.

| File | Contents and publication point |
|---|---|
| `capture.json` | Existing dimensions, pixel format, wavelength axis and preview fields, plus a versioned `recording_contract`. Written before the first segment. |
| `device.json` | One snapshot after receive buffers are allocated and before `StreamEnable`/`AcquisitionStart`: identity/firmware nodes, ordered configuration requests, attempted values after clamping, immediate readbacks, optional-write failures, final readable settings, transport, actual buffer allocation and socket readback, calibration reference and timing intent. Published atomically without overwriting an existing snapshot. |
| `telemetry.jsonl` | `fx10-telemetry-v1` rows at start (before acquisition), each existing 60 s device-poll tick and stop (or `startup-failed` during partial startup). ProcPCB/FPGA temperatures, raw Counter1 value, baseline, delta, binding verification and regression flag. Each row includes host realtime/monotonic read-start and monotonic read-finish times and whether acquisition was started. |
| `stream_stats.txt` | Full readable `PvStream` parameter dump after receive-thread shutdown and before stream disconnect. Unreadable/error values are explicit. This is an SDK snapshot, not the application's frame ledger. |
| `segment_NNNN.lines.csv` | `fx10-line-index-v2`: original ten columns retain their order/meaning; twelve appended columns retain SDK layout and operation metadata for accepted and rejected buffers. |
| `segments.jsonl` | `fx10-segment-v1`: one row after a segment's BIL/index and `.hdr` have been published. Counts actual frames separately from synthetic padding, records gap/rejection/anomaly counts, byte sizes, global line interval and first/last actual frame identities/timestamps. |

The twelve new index columns are `sdk_acquired_size`, `sdk_payload_type`,
`sdk_operation_result`, `sdk_chunk_count`, `sdk_image_present`, `sdk_pixel_type`,
`sdk_width`, `sdk_height`, `sdk_padding_x`, `sdk_padding_y`, `sdk_image_size`, and
`sdk_effective_image_size`. Empty means unavailable; padding/gap rows have no
SDK fields. Non-image buffers retain generic SDK fields but leave image fields
empty. Pixel type and operation result are opaque SDK numeric codes. The app's
live and snapshot readers accept both v1 (10 columns) and v2 (22 columns).

Packed pixels are unpacked to uint16, and SDK-declared padding is omitted.
Recording layout metadata does **not** recover the omitted bytes or unused packed
bits: ENVI preserves the canonical pixel values, not a byte-exact SDK payload.
Rejected buffers retain diagnostic metadata, not their pixel bytes. `frame`
means one spatial scan line with all delivered image rows, not an instantaneous
full-scene hyperspectral cube. `EnStatusLine` replaces an image row when enabled;
it must not silently become a wavelength-calibrated band.

Unknown nodes use `{value: null, status: ...}`. AIE remains unknown (no verified
node name). `sdk.header_version` comes from `PvVersion.h`'s `NVERSION_STRING`
(verified in the bundled 6.5.1.6797 SDK headers); `sdk.runtime_version` is null
because the loaded binary version is not inferred from build-time headers.
Camera firmware/version readbacks are separate. `factory_preparation.completed` records acknowledged
commands and subsequent control readiness, not proof of every factory setting.
MROI writes retain their selector context through the ordered apply log; that
log is not a reread of every region after arbitrary raw overrides.

Telemetry reuses the existing monitor-thread reads; it adds no device reads in
the image callback. Its persistence is independent of `logging.stats_interval_s`.
The start sample establishes a baseline before this process enables SensorSync
pulses. The raw counter is not a trigger total. Delta is null unless the initial
`Counter1_EventSource` readback confirms `MissedTrigger` and the counter baseline
and current value are usable. A regression is latched (reset versus wrap is not
guessed); a verified missed-trigger counter regression ends the session as an
integrity failure. Host times and raw SDK timestamps remain observations: no UTC
epoch or trigger/BlockID association is inferred. `sensor_trigger.log` and a
separately verified association anchor remain necessary for the intended timing.

Metadata I/O failures are visible and prevent a successful main-session result.
Device metadata must publish before acquisition begins. Low-frequency JSONL rows
are fully written and `fdatasync`ed; failed partial appends are truncated to the
last complete prefix where possible, then latched against retries. Stream text
is checked on close but has no separate durability barrier. Segment summaries
are synced during existing segment finalization, not on every image line.

The `.hdr` remains the authority for a finalized ENVI segment. `closed_clean`
describes finalization only, **not** absence of lost frames. A summary can be
missing after a crash or a summary-write failure even when `.hdr` exists; scan
the files and index rather than treating the manifest as an exhaustive inventory.
An event-only segment has zero BIL lines and null first/last frame identities.
Gap counts belong to the segment containing the gap event; its synthetic padding
can span later segments. Global line intervals are zero-based, end-exclusive.

Verification to execute separately: SDK-free metadata/index/recorder tests,
legacy and v2 preview reads, hardware node availability and readbacks, an idle
external-trigger run, a run shorter than 60 s, disconnect/disk-full shutdown,
and capture throughput with metadata enabled. These checks have not been run as
part of this implementation's static-only validation.

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
* **Mechanical-shutter completion timing.** The operator supplied the numeric
  `MotorShutter_PulseRev` / `MotorShutter_PulseFwd` actions (255 opens/closes),
  but their stored values cannot confirm position. Actual motion-completion
  feedback and any required settling interval remain unspecified; the driver
  records only acknowledgement of the explicit open/close writes.
