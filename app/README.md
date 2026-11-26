# Phenomate web control panel (`app/`)

`app/` is the NiceGUI control panel of PhenomateDrivers, shown as "Phenomate" in the browser. It
runs in the `phenomate-drivers-dev` container beside the C++ acquisition binaries, controls
recordings and camera tools, and parses recorded data for GUI previews only. Offline export,
format conversion and calibration belong to the separate PhenomateDecoders project; this project
provides no command-line tools for them. Paths are relative to the repository root.

## Runtime and pages

`app/main.py` runs from `/workspace` as `python3 -B -m app.main` and serves `/healthz`,
`/preview-image/{key}` and the pages on `PHENOMATE_GUI_HOST:PHENOMATE_GUI_PORT` (default
`0.0.0.0:8619`): Overview (`/`), Data preview (`/live`), Navigation & IMU (`/asterx`), Camera
workbench (`/camera`), Reference capture (`/reference`), Configuration (`/config`, raw-text YAML
with syntax check only), System logs (`/logs`) and Recording history (`/sessions`).
`app/_runtime/` (`PHENOMATE_RUNTIME_DIR`) holds `controller.lock`, `tool-operation.json`,
`acquisition.json`, the newest 10 snapshots per camera type (`SNAPSHOT_KEEP`) and focus streams.

## Module layout

| Location | Responsibility |
| --- | --- |
| `app/constants.py`, `app/state.py` | Paths, config registry, `PHENOMATE_*` settings; UI state `STATE`, whose recording fields only `AcquisitionController` changes |
| `app/decoder/ebus/` | `ebus-raw` reader; header, metadata and sequence checks on every read, `session_end` counters for finalized reads, payload CRC-32C on request (`verify_crc`); PFNC unpacking (`pfnc.py` registers only the 7 formats in use) |
| `app/decoder/exposure.py` | ETTR arithmetic shared by GOX and FX10: linear fit, suggestion, frame-period cap, verification |
| `app/decoder/gox/` | Native images, preview encoding, focus metric, chart detection and fit, color values, ΔE00, ETTR points |
| `app/decoder/fx10/` | Profile and cube reading, raw-DN band statistics, focus metric, white-reference ETTR points |
| `app/decoder/asterx/` | Session CSV telemetry, incremental reading, static route for History |
| `app/services/` | Process and tool lifecycle, configuration, session state, live previews, history scheduling |
| `app/ui/` | Pages, chart options and result display |
| `app/tests/`, `app/resource/` | `unittest` sources; stylesheet, icons, chart layout images |

In `app/services/`, `process.py` (`AcquisitionController`) owns the acquisition and `tool_jobs.py`
the device tools. GoX and FX10 share `camera_operations` (camera reservation), `snapshot_tool`,
`timing`, `exposure_series`, `focus_stream`, `reference_storage` and `preview_images`; `gox_chart`
serves the GOX reference and GOX exposure series. `app/ui/components.py` holds shared widgets
(`info`, `hint`, `heading`, `preview_image`, `metric`, `raw_output`, `confirm`, `busy`) and copy;
`app/ui/ettr_section.py` is the ETTR panel of both references. There is no LMS payload decoder.

## Dependency direction

Imports run `app/main.py` → `app/ui/` → `app/services/` → `app/decoder/`; `app/ui/` also imports
decoder data types, constants and pure helpers, and `app/decoder/gox/` and `app/decoder/fx10/`
build on `app/decoder/ebus/` and `app/decoder/exposure.py` (`app/decoder/fx10/focus.py` also reuses
`png_bytes` from `app/decoder/gox/preview.py`). `app/decoder/` imports neither `app/services/`,
`app/ui/` nor `app/state.py`, controls no device, starts no process and never modifies recordings.
Chart options stay in `app/ui/`; request exclusion, cancellation, session changes and result
publication stay in `app/services/`. New sensor algorithms belong in `app/decoder/<sensor>/`.

## Data-protection invariants

These rules protect recorded data or configuration and are kept through any simplification:

- Camera exclusion: `camera_operations.guard_reason` and the `STATE.snapshot_busy` reservation; the
  enables captured at recording start (`enables_at_start`) define the cameras a recording owns.
  Discovery is always allowed; FX10 reference collection requires the whole rig to be idle.
- Device tools: `tool_jobs.check_idle` (`pgrep` per tool name), the fsynced journal
  `tool-operation.json`, a launch gate that starts a tool only after its PID is journaled, and
  stopping by journaled process identity (TERM, then KILL after 15 s).
- Acquisition: TERM only, never KILL; GUI shutdown waits until acquisition and tools have exited.
  Reattachment requires the identity triple (PID, start ticks, boot id) and the process's own
  manifest, never the newest folder; a run is clean only if its final manifest says so.
  `control_owner` keeps one controller through an `flock` on `controller.lock`.
- Configuration writes: `_require_writable` refuses saves while a tool runs or its exit is unknown,
  while acquisition ownership is unverified, and from Start until the manifest reports
  `configuration_read` in phase `running` (again from `stopping`). Saves detect path and mtime
  conflicts, keep a `.bak` and replace atomically. Apply reads every written value back
  (`_verified_text`), writes recording and snapshot configs as a staged pair with rollback, and
  refuses a `.bak` path that overlaps an active config.
- Retention: references and exposure series stay in `<Output Directory>/tmp/` until saved;
  `reference_storage.save` never replaces an existing directory; failed or interrupted references
  keep their RAW and a failure record. ColorChecker calibration verifies its frame's payload
  CRC-32C (`source_payload_crc32c_verified`); FX10 pairs are checked against `session_end`.

## C++/GUI contract strings

These strings are shared with the C++ side and never change on one side only:

- Run manifest `status_schema` `phenomate-run-v1` (`run_status.SCHEMA`), in
  `<session>/raw/drivers.json` and in `app/_runtime/acquisition.json` (passed as
  `PHENOMATE_STATUS_FILE`). `HISTORY_SCHEMAS` adds the legacy `amiga-run-v1` for History only.
- Tool markers `SNAPSHOT: OK <dir>` / `SNAPSHOT: FAIL <code> <reason>` (read before the exit code);
  `REFERENCE: CONFIG`, `SESSION`, `PHASE white|dark`, `OK`, `FAIL`; manifest `fx10-reference-v1`.
- Binaries in `build/bin/`: `PhenomateDrivers`, `jai_snapshot`, `fx10_snapshot`, `fx10_reference`,
  `ebus_discover`, `ebus_set_ip`. `runtime._pgrep_args` truncates names to the 15-character kernel
  process name for `pgrep -x`.
- Environment: `PHENOMATE_` + `MAIN_CONFIG`, `SNAPSHOT_CONFIG`, `FX10_SNAPSHOT_CONFIG`, `GUI_HOST`,
  `GUI_PORT`, `RUNTIME_DIR`, `CONTROL_LOCK`, `CONFIG_ROOT`, `DATA_ROOT`; no `AMIGA_*` fallback.
- `[Statistics]` `fps=` fields and module tags parsed by `app/services/driver_stats.py`, checked by
  `tools/check_contracts.py`.

## GUI text rules

1. A section has a title and at most one visible hint (about 60 characters, English phrase);
   rationale, formulas and sources go behind ⓘ (`components.info`, click or tap, ≤ 3 sentences),
   the full explanation in this file.
2. Numbers come from constants, e.g. `f"{timing.PERIOD_FRACTION:.0%}"` or `ettr.TOLERANCE`.
3. Shared copy is defined once (`components.NEXT_RUN`, `components.PREVIEW_DETAILS`).
4. A notification is one sentence (about 80 characters) followed by the raw error.
5. The UI shows no internal names (`PhenomateDrivers`, `phenomate-run-v1`, `build/bin`, function
   names); the platform name is Phenomate.

## Data formats and display

- Camera input is `ebus-raw-eBUS_SDK_v<version>` with device metadata
  `ebus-device-eBUS_SDK_v<version>`; `<version>` equals `sdk.header_version`, the build-time
  `PvVersion.h` `NVERSION_STRING` (e.g. `6.5.1.6797`). The binary header layout version (`1`) is
  checked separately. Unsuffixed or `-v1` names, legacy JAI RAW and FX10 ENVI/BIL are neither read
  nor converted; field definitions are in `ebus_driver/README.md`.
- GOX accepts only `BayerRG12Packed` (PFNC `0x010C002B`, 2 px in 3 B); the driver requires readbacks
  `PixelFormat=BayerRG12Packed` and `SensorDigitizationBits=12`. RAW keeps the SDK bytes; lossless
  unpacking gives 12-bit DN (0–4095) in `uint16`. A 4128 × 3008 frame is 18,625,536 B, 3/4 of
  unpacked `BayerRG12`. The focus stream is the only `BayerRG8` exception.
- GOX preview: OpenCV bilinear demosaicing in `uint16`; the screen image is linear DN / 4095,
  area-downscaled to ≤ 1600 px (`DISPLAY_LONG_EDGE`), 8-bit PNG; the download is a full-resolution
  16-bit PNG with `DN << 4`, decoded from RAW on first request. Histogram and clipped fraction use
  native photosites. Previews are neither measured nor saved to disk.
- FX10 statistics are raw DN without normalization, dark subtraction or reflectance conversion;
  saturation is a percentage, and full-scale guides follow the recorded PixelFormat. Without
  recorded wavelengths, bands are spread evenly on 400–1000 nm (uncalibrated).
- AsteRx pages read the session's `live_insnavgeod.csv` and `live_receiverstatus.csv`
  (`host_unix_ns` is the host clock). History shows sessions and a zoomable route map, without
  playback, per-sample history or export. ENVI is neither read nor written.

## Camera workbench

Discovery also runs during a recording and marks owned cameras "in use"; Set IP can update
`device.ip` in the driver config once the address is persisted. Take snapshot passes workbench
values as overrides: `--exposure-ms`, `--gain`, `--frame-rate-hz` to `jai_snapshot` (one GOX
frame); `--exposure-ms`, both binnings, `--fps` and `--frames` = `max(1, round(rate))` to
`fx10_snapshot` (about 1 s), which revalidates and never lowers the rate silently.

Apply to config writes GOX exposure, frame rate and gain, or FX10 exposure, frame rate and both
binnings, to the recording config and the snapshot config. A GOX entry is matched by MAC, else by
IP, and must be `cameras[0]` in the snapshot config. Applying an ETTR suggestion writes only the
exposure. Snapshot configs are not shown on the Configuration page.

The Frame rate field follows exposure and binning to the highest valid rate, floored to 0.001 Hz
(`RATE_STEP_HZ`); the operator can only lower it. `app/services/timing.py` and
`app/services/sensor_sync_rates.py` mirror `gox_driver/include/app_config.h`,
`fx10_driver/include/app_config.h` (`CheckAcquisitionTiming`) and
`common/include/sensor_sync_rates.h`. With an external trigger and SensorSync, `frame_rate_hz` is
the pulse rate, which the board's PWM must be able to produce.

| Limit | GOX | FX10 |
| --- | --- | --- |
| `frame_rate_hz` | 0.125–6.054 Hz (`GOX_MIN/MAX_FRAME_RATE_HZ`) | ≤ 163.02 Hz at 1 × 1 binning, else ≤ 324.373 Hz |
| `exposure_ms` | ≤ 399.405 ms (`GOX_MAX_EXPOSURE_MS`, ExposureTime readback) | 0.01–419 ms (`FX10_MIN/MAX_EXPOSURE_MS`) |
| Exposure vs period | ≤ 90 % of the period, inclusive (`PERIOD_FRACTION`) | same |
| SensorSync pulse rate | JAI group 0.140–9847.689 Hz | FX group (channels 0/1) 17.882–9847.689 Hz; channels 2/3 as JAI |

## Focus assist

Both streams hold the camera reservation. `app/services/focus_stream.py` launches the tool,
follows the newest committed records, deletes closed segments behind the newest 3
(`KEEP_SEGMENTS`) and stops the tool with SIGTERM. The next stream replaces the stream directory
(`app/_runtime/focus/`, `focus_fx10/`); afterwards only `device.json` and `events.jsonl` remain,
and no configuration is written. Recording, snapshots, references and config saves stay locked
until Stop, the header's Stop tool, 1800 s (`MAX_STREAM_S`), a failure, or 15 s without an open
Camera workbench page (`HEARTBEAT_TIMEOUT_S`). Workbench changes apply only after a restart.

### GoX

- Stream: `jai_snapshot --stream-s 1800`, GOX snapshot config in freerun (PTP and SensorSync off),
  workbench camera, exposure and gain; `BayerRG8`, no frame limit, one record per segment, writer
  queue for about 2 s of write stall. Only the code flag `AcquisitionConfig::focus_preview` enables
  `BayerRG8`; YAML cannot. The A/D depth keeps its factory value and must read back 10 or 12 bit;
  8-bit digitization would raise sensitivity 4× (p. 45), so workbench exposures would not transfer.
- Rate: `floor(min(9.078, 900 / exposure_ms), 0.001)` Hz, independent of the Frame rate field.
  The camera displays 9.079 Hz, but GenApi rejected 9.079 on hardware (2026-11-17), hence
  `GOX_MAX_FOCUS_FRAME_RATE_HZ` = 9.078 (C++ `gox::kMaxFocusFrameRateHz`).
- Metric (`app/decoder/gox/focus.py`): Gr photosites (row 0, column 1 of each 2 × 2 tile) from the
  8-bit payload, without demosaicing; `score = var(Laplacian(G)) / (mean(G) − 8)²`. 8 DN is the
  documented black level 8LSB@8bit (p. 172), 128 DN at 12 bit; the Laplacian ignores it, and the
  normalization removes the dependence on light and exposure. ROIs less than 4 DN above it
  (`MIN_SIGNAL_DN`) are not scored. The display is the median of 3 frames as a percentage of the
  session peak, reset by moving or resizing the ROI, Reset peak or restart. More than 0.5 % ROI
  photosites at 255 raise a warning, since clipped edges inflate the score.
- View and procedure: clicking the overview places the ROI and clicking the magnifier recenters it;
  the magnifier shows one image pixel per Gr photosite without smoothing (the central 256 × 256 of
  an ROI wider than 512 px), as linear DN / 255. Set the exposure so the ROI does not clip, turn the
  ring slowly past the peak, return to ≥ 95 %, lock and confirm ≥ 95 %. Reset the peak after a
  change of target height, aperture or light.

### FX10

- Stream: `fx10_snapshot --stream-s 1800`, FX10 snapshot config in freerun (SensorSync off),
  workbench camera, exposure, binnings and frame rate checked as for Take snapshot; segments of
  `max(1, ceil(rate × 0.25))` records. `--frames` and `--stream-s` are mutually exclusive.
  `app/services/fx10_focus.py` averages the newest ~0.2 s of frames (≤ 16) at most every 0.25 s.
- Metric (`app/decoder/fx10/focus.py`, ported from `resource/fx10e_focus_monitor.py`, no spatial
  ROI): bands are averaged in 28 groups (8 bands each at the factory 2× spectral binning). Edges
  are found on the mean profile of the 450–900 nm groups; only isolated edges with flat plateaus on
  both sides are kept, which excludes bar ladders and V-slit locators. Per group, each edge is
  recentered on its 50 % crossing, normalized to 0..1 by its plateaus and stacked into one
  oversampled ESF; a Gaussian ESF fit gives the LSF FWHM in pixels (lower is sharper; pixel
  sampling sets a floor of about 1.0–1.3 px). The focus value fits all 450–900 nm edges jointly;
  L · C · R fit each third of the line.
- Display and procedure: median of 3 measurements; "% of session best" = 100 × best / current,
  reset by Reset best or restart; warnings above 0.5 % clipped pixels or below 4 edges. The frame
  uses eBUS Player orientation (x = spatial pixel, y = band) at linear DN / full scale, with
  measured edges ticked; the 128 × 64 magnifier is display only. Focus coarsely by eye until edges
  are ticked, then proceed as for GoX.

## Reference capture

References use `config-fx10-snapshot.yaml` and `config-gox-snapshot.yaml` in freerun without PTP,
hardware trigger or SensorSync, never the recording configs or workbench values. Results stay in
`<Output Directory>/tmp/` until the page's Save button moves them into `<Output Directory>/`.

### FX10 white and dark references

`fx10_reference` captures white (shutter open), then dark (shutter closed), each for
`reference.duration_s` (driver default 5 s). The GUI accepts the pair only if `reference.json`
reports `fx10-reference-v1` and `completed` and both phases' counters match their `session_end`.
Per-band mean, median, min, max and p90 over all valid frames are plotted; a plotting error never
fails the pair. Saving adds `saved.json` and leaves `reference.json` unchanged.

### GOX ColorChecker reference

The operator selects MCC24, SG140 or VINYL18 (`cv.mcc` types) by clicking its layout image; nothing
is preselected. Capture & calibrate requires OpenCV 5 with `mcc` and `ccm` (`capability_reason`;
nothing is installed) and captures one frame with the snapshot config's device, exposure and gain.
`cv.mcc.CCheckerDetector` returns up to 6 candidates; because regular textures also pass detection,
the operator selects the chart among outlined thumbnails or rejects all.

Detection uses a temporary 8-bit image, the only bit-depth exception, which enters neither patch
means, fit nor previews; these use full-resolution 12-bit DN demosaiced in `uint16`. `cv.ccm` fits
a linear 3 × 3 matrix (identity linearization) from normalized linear camera RGB to linear sRGB
(D65). Calibration fails unless the device audit records `LUTMode`, `GradationCompressionMode`,
`ColorTransformationRGBMode` and `BalanceWhiteAuto` as Off, no patch clips or reaches 98 % of full
scale, and every patch is fitted. The corrected preview clips linear sRGB to [0, 1] and applies the
sRGB OETF only.

The fit loss is the RMS CIEDE2000 of `cv.ccm`. Per-patch ΔE00 compares the OpenCV reference linear
sRGB with the pedestal-subtracted patch mean `@ matrix`, both in CIE L\*a\*b\* (D65/2°,
kL = kC = kH = 1); its RMS over fitted patches equals the loss.

`reference_gox_<UTC>_<ID>/` holds `raw/gox/` (unchanged `ebus-raw`), `raw/config/` (config copy)
and `reference.json` (`gox-color-reference-v1`: chart, candidates, selection, matrix, fit metrics,
provenance, relative RAW paths). Previews exist only in the GUI; the matrix is not written to the
camera or applied to later recordings.

### Color calibration values and black level

`app/decoder/gox/color.py` supports fitting, storing and previewing only; applying a matrix to
other frames (exposure-ratio scaling, batch correction) is done offline by PhenomateDecoders.

- `CameraRGBNormalization`: `dn_over_full_scale` (DN / 4095) or `pedestal_subtracted`
  ((DN − B) / (4095 − B), scalar B `measured` or `documented`); the reference fit uses 128 DN.
- `normalize_camera_rgb` converts `uint16` RGB to unclipped `float32` (values below B stay
  negative); `normalize_patch_means` applies the same formula to `float64` patch means.
- `ColorCalibration.to_dict()` writes `schema_version`, `calibration_id`, `matrix`,
  `matrix_convention` (row vector `camera_rgb @ matrix → linear sRGB D65`), `normalization`,
  `applicability` (device, integration time, gain, pixel format, linear audit,
  `exposure_compensation`), `fit` and `source`; the matrix must be finite 3 × 3, full rank, with
  condition number ≤ 1e6.
- `frame_exposure_from_device` reads the integration time (`derived.actual_integration_us`) and the
  last `Gain` readback from `device.json`; a missing value is an error, never ×1.

The black level 8LSB@8bit (GOX manual Ver. 1.3, p. 172, "Black Level Adjustment") is scaled by 16
to 128 DN at 12 bit (`DOCUMENTED_PEDESTAL_DN`, `app/decoder/gox/recording.py`). No register reports
it, so it is documented, not measured. Calibration subtracts it before fit and correction and warns
if `device.json` `derived.black_level` does not derive 128 DN or the `BlackLevel` register is not 0;
a dark frame is then required.

### ETTR exposure suggestion

The measured peak is linear in exposure: `peak_dn = offset + slope × (t × g)`, with `t` the actual
integration time and `g` the gain (1 for FX10). A bracket around the initial exposure (GOX ±50 ms,
FX10 ±15 ms; the low point is half the initial value if that does not exceed the step) is fitted on
points below 98 % of full scale (`NEAR_SATURATION`). GOX bracket captures run at the highest rate
their exposure allows, FX10 captures at the lower of that rate and the configured rate. The target
is the brightest patch's brightest channel at 80 % of full scale (GOX) or the brightest band's p90
at 10–95 %, default 60 % (FX10). A suggestion above the frame-period limit is capped at 98 % of it
(`LIMIT_MARGIN`); gain and binning never change. The next reference capture verifies it: within
±5 % (`TOLERANCE`) it passes, otherwise all points are refitted; a saturated verification yields
only a conservative decrease.

## Tests

Tests are in `app/tests/` (`unittest`); color-calibration numerics are covered by
`test_gox_color.py` and `test_gox_delta_e.py`. `app/tests/` has no `__init__.py`, so modules are
named explicitly, from `/workspace` in the container:

```bash
docker exec -w /workspace phenomate-drivers-dev bash -lc \
  'python3 -B -m unittest app.tests.test_timing app.tests.test_gox_color'
docker exec -w /workspace phenomate-drivers-dev bash -lc \
  'python3 -B -m unittest $(ls app/tests/test_*.py | sed "s|\.py$||; s|/|.|g")'
```

## Limitations

Implemented but not verified end to end on hardware: GoX focus assist (including whether 9.078 Hz
is writable and sustained in `BayerRG8` at factory A/D depth), FX10 focus assist, and the complete
ColorChecker capture and calibration workflow. The display bit depth of browser, compositor and
monitor is not verified.
