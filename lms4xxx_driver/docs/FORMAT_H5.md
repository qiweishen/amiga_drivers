# lms4xxx recording format: `lms4xxx-h5` (HDF5)

Every LMS4xxx instance of a run records its scans into HDF5 files:

```
<Output Directory>/<YYYYMMDD_HHMMSS>/raw/lms4xxx/scan_<id>_<YYYYMMDD_HHMMSS>_NNN.h5
```

`NNN` is the split index (`000`, `001`, …): a new file starts whenever
`output.max_file_bytes` of uncompressed payload were appended (1 GiB by
default, ~5 minutes at 600 Hz with the default channels). All split files of
one instance carry the identical layout and continue the same scan sequence;
concatenate them in index order to get the whole run.

The files are self-describing: every dataset and attribute below can be
discovered with `h5ls -r`, `h5dump -A` or `h5py` without any code from this
repository. The producer is `lms4xxx_driver/src/lms4xxx_scan_h5_file.cpp`;
`lms4xxx_driver/scripts/inspect_h5.py` is a reference reader (summary,
integrity check, CSV export).

## Layout

```
/                                   root attributes: format, format_version, producer, instance_id,
│                                   session_timestamp, split_index, channel_mask,
│                                   channels, points_per_scan, config_*_deg, config_output_rate,
│                                   config_remission, device_firmware, device_order_number,
│                                   device_type, device_name, device_keeps_flagged_points,
│                                   device_* audit (see below), compression, swmr,
│                                   hdf5_library_version, layout, config_yaml (only when readable),
│                                   closed_cleanly + frames_total (written at close)
├── frames/                         per-scan metadata — one 1-D dataset per field, row i = scan i
│                                   (group attribute: description)
│   ├── device_time_unix_us int64   telegram timestamp as µs since the epoch (device clock), 0 if absent
│   ├── time_since_startup_us, transmission_time_us          uint32, µs of device uptime
│   ├── telegram_counter, scan_counter, num_points           uint16
│   ├── start_angle (int32), angle_step (uint16)             1e-4 deg
│   ├── scan_frequency (1/100 Hz), measurement_frequency     uint32
│   ├── device_version, device_number, serial_number, device_status_1/2
│   ├── digital_input_1/2, digital_output_1/2
│   ├── has_encoder, encoder_position
│   ├── has_timestamp, ts_year, ts_month, ts_day, ts_hour, ts_minute, ts_second, ts_microsecond
│   ├── y_rotation (float32)
│   └── has_device_name, device_name (fixed 16-byte string)
└── channels/                       raw device values — one 2-D [scan, point] dataset per recorded channel
    │                               (group attributes: description, angle_formula, points_per_scan)
    ├── dist   uint16 [N, 841]      DIST1  scale_factor 0.1,  offset 0       → mm
    ├── rssi   uint16 [N, 841]      RSSI1  scale_factor 1.0,  offset 0       → digits
    ├── refl   uint16 [N, 841]      REFL1  scale_factor 0.01, offset 0       → percent
    ├── angl   uint16 [N, 841]      ANGL1  scale_factor 1.0,  offset -32768  → 1e-4 deg
    └── qlty   uint8  [N, 841]      QLTY1  quality bitfield
└── telemetry/                      device self-report, one row per poll — INDEPENDENT of the scan index
    │                               (group attribute: description)
    ├── unix_us        uint64       CLOCK_REALTIME µs when the answer was decoded
    ├── temperature_c  float32      internal device temperature; NaN = the device did not answer
    ├── device_state   int32        0 busy / 1 ready / 2 error; -1 = not answered
    ├── warning_count  uint32       number of active device warnings or errors
    └── warnings       string       clear text, '; '-separated (sRN EMActiveCustomerInfo)
```

`dist`, `angl` and `qlty` are always recorded; `rssi` or `refl` follows
`scan.remission` in the driver config (the device streams one remission
channel; root attributes `channels` and `config_remission`). The device is
configured explicitly on every start — see `DEVICE_CONFIG.md` for the exact
telegrams; `device_*` attributes carry firmware, order number, type and the
device name (`LocationName`), and `device_keeps_flagged_points` is 1 for the
LMS4124R-13000S01 variant, whose firmware still outputs points flagged by
quality bits 1–4 as valid. Every dataset shares the scan index: row `i` of
`/frames/*` describes row `i` of each `/channels/*` dataset.

Each `/frames/*` dataset carries `description` (and `unit` where applicable)
attributes; each `/channels/*` dataset carries `content`, `scale_factor`,
`scale_offset`, `unit`, `description` and `channel_mask_bit`.

### Physical values

```
physical            = raw * scale_factor + scale_offset            (attributes of the dataset)
distance_mm         = dist * 0.1
reflectance_percent = refl * 0.01
angle_correction    = (angl - 32768) / 1e4                          [deg]
angle_deg[scan, i]  = (frames/start_angle[scan] + i * frames/angle_step[scan]) / 1e4
                      (+ angle_correction[scan, i] where /channels/angl is recorded)
```

`dist` raw values `0..15` are not measurements: `0` = too dark / out of range
/ filtered, `1` = reflection too strong, `2..15` reserved. `qlty` bits: `0x01`
below signal lower limit, `0x02` above signal upper limit, `0x04` below
distance lower limit, `0x08` above distance upper limit, `0x10` normal
measurement, `0x20` edge hit possible, `0x30` edge hit likely, `0x40`
suspected outlier, `0x80` partial gloss.

Rows are zero-padded beyond `frames/num_points[i]` (always 841 for the
default 55°…125° window at 1/12°).

The scale attributes carry the values documented by SICK for the LMS4xxx; the
recorder compares them exactly with the factors reported in every telegram.
A mismatch or missing channel rejects the scan and fails the run; accepted
device factors therefore equal the stored attributes. Geometry and payload
counts are also checked on every scan before conversion to the fixed layout.

### Timestamps

No host-computer time is recorded anywhere in the file: the host clock is not
trusted on the platform. Existing device-clock fields below are diagnostic
only, not synchronization sources. Cross-sensor association uses AsteRx GPS
time and SensorSync driven by AsteRx PPS/ZDA, with the ZDA time-scale conversion
explicitly established. There is no host/device-clock fallback; tests without
SensorSync do not provide cross-sensor time association.

* `device_time_unix_us` — the telegram's timestamp (year…microsecond fields
  combined, UTC), NTP-synchronised (see `ntp=` in the status line). Scans are
  only written after the driver's time lock: the first recorded scan is the
  first one whose time stamp is at or after 2026-01-01 (the device streams a
  free-running 1970-epoch clock before its first NTP sync), and from then on
  the device time must advance in step with `time_since_startup_us`
  (`ntp.max_time_step_ms`, `DEVICE_CONFIG.md` "Time") or the run faults. With
  NTP off (a warned configuration) the device clock free-runs and the driver
  switches the time stamp block off (`has_timestamp` = 0,
  `device_time_unix_us` = 0, `ts_*` = 0).
* `time_since_startup_us` — device uptime, wraps every 2^32 µs (~71 min);
  `inspect_h5.py` uses it for the span/rate estimate.

## Reading

Python (`pip install h5py numpy`):

```python
import glob, h5py, numpy as np

files = sorted(glob.glob("raw/lms4xxx/scan_Front_Right_Laser_20260806_070253_*.h5"))
with h5py.File(files[0]) as f:
    print(dict(f.attrs))                       # instance_id, channels, config_yaml, ...
    f.visit(print)                             # every dataset path
    dist = f["channels/dist"]                  # h5py.Dataset, lazily read
    mm = dist[:1000] * dist.attrs["scale_factor"] + dist.attrs["scale_offset"]
    t = f["frames/device_time_unix_us"][:]     # int64, µs since the Unix epoch (device clock)
    n = f["frames/num_points"][:]              # valid points per scan
    start = f["frames/start_angle"][:] / 1e4   # deg
    step = f["frames/angle_step"][:] / 1e4     # deg
    angles = start[:, None] + np.arange(dist.shape[1])[None, :] * step[:, None]
```

Whole run across split files (`h5py` virtual datasets or simply concatenate):

```python
dist = np.concatenate([h5py.File(p)["channels/dist"][:] for p in files])
```

Command line: `h5ls -r scan_…_000.h5`, `h5dump -A scan_…_000.h5` (attributes
only), `h5repack -f GZIP=4 in.h5 out.h5` (compress a recording offline).

## Writer behaviour

* Datasets are chunked (`chunk_frames` scans × 841 points on the channels,
  4096 scans on the metadata columns) with unlimited length; the write thread
  appends the queue in `chunk_frames` batches and calls `H5Fflush` every
  `flush_interval_ms`. The file uses the HDF5 1.10 format
  (`H5F_LIBVER_V110`), readable by every HDF5 ≥ 1.10 / h5py ≥ 2.x.
* `compression_level` 1..9 enables shuffle + gzip on the channel datasets
  (typically 1.5–2× smaller for lidar distance data). Metadata columns stay
  uncompressed. The recorder falls back to uncompressed output with a warning
  if the libhdf5 build lacks the deflate filter.
* `swmr: true` switches the file into single-writer/multiple-reader mode after
  the layout is created: other processes can open it read-only
  (`H5F_ACC_SWMR_READ`, `h5py.File(p, "r", swmr=True)`) while it is being
  written and see every flushed row. Requires a local POSIX file system (not
  NFS/SMB).
* Crash semantics: HDF5 has no journal, so a file whose writer is killed
  between flushes may be unreadable. Each flush is `H5Fflush` **followed by
  `fdatasync` on the underlying descriptor**, so the exposure really is bounded
  by `flush_interval_ms` and not by the kernel's writeback window; without the
  fdatasync a power cut could cost tens of seconds regardless of the setting.
  Three things are still lost on a `kill -9`: the scans appended since the last
  flush, the partial batch (< `chunk_frames`) waiting in the write thread, and
  whatever sits in the SPSC queue. Only the current split is at risk; earlier
  splits are closed. `h5clear -s` (HDF5 tools) can remove the status flags of a
  file left open by a crashed SWMR writer.
* **Completeness marker.** HDF5 offers no atomic "rename on finish", so a
  finished file is marked instead: at close the recorder writes the root
  attributes `closed_cleanly = 1` and `frames_total = <rows>`. **A file without
  `closed_cleanly = 1` is incomplete**. A latched append/flush failure writes
  `closed_cleanly = 0`; `frames_total` is the fully appended row prefix
  (not a guarantee of persistence),
  even if a failed batch left datasets unevenly extended. A failed close
  fails the run. The marker alone cannot prove that the final close or storage
  persistence succeeded; check the run outcome and dataset lengths as well.
  `scripts/inspect_h5.py verify` checks the marker.
  Files written with `swmr: true` carry no marker: SWMR forbids adding
  attributes after the layout is frozen.
* An existing file is **never** overwritten (`H5F_ACC_EXCL`). The path already
  contains the session timestamp and the split index, so a collision means the
  run layout is wrong, and destroying an earlier recording would be the worst
  possible response to that.
* The writer's counters appear in the periodic `[Statistics]` line as
  `fps=` (scans **written to disk** per second — the meaning
  `app/services/driver_stats.py` documents and the dashboard cards show for every
  driver; it used to count queue acceptance here, which is now the separate
  `queued=` field), `written=` (total on disk), `drop_q=`, `files=` and `bytes=`,
  and in the shutdown `Final:` line as `frames_written=`, `dropped_queue=`,
  `bytes=`, `files=`. `bytes=` counts logical (uncompressed) payload as it is
  stored: the packed `/frames` row plus 841 points per recorded channel. It is
  computed from the file types, not from `sizeof(FrameMeta)`, which carries
  alignment padding that never reaches the file. A write failure
  (HDF5 error, disk full, failed split rotation) stops the writer and
  terminates the run, like a driver fault.

## Format history

* v2 (2026-08): root attributes `config_remission`, `device_firmware`,
  `device_order_number`, `device_type`, `device_name`,
  `device_keeps_flagged_points`; the device name block is switched off
  (`has_device_name` = 0, `device_name` empty — the serial number is in
  `frames/serial_number`, the `LocationName` in the root attribute). The v1 host-time columns (`host_time_unix_us`,
  `host_time_mono_us`) and the `created_utc` attribute are gone: the host
  clock is not trusted.
* `lms4xxx-h5` v1 (2026-08) replaces the private `scan_*.bin` container
  (`LMS4` magic, packed 96-byte record header, fixed 841-point channel slots)
  that needed the repository's `DataConverter` to be read. The `.bin` layout
  did not store the channel scale factors or any host timestamps.
