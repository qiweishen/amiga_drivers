# eBUS camera runtime and RAW format

`ebus_driver/` contains the acquisition engine shared by the JAI Go-X and Specim FX10e camera
profiles and the SDK-free `ebus-raw` recorder. The engine uses the Pleora eBUS SDK 6.5.1
(`.devcontainer/Dockerfile` installs 6.5.1-6797). Device configuration is described in
[gox_driver/docs/DEVICE_CONFIG.md](../gox_driver/docs/DEVICE_CONFIG.md) and
[fx10_driver/docs/DEVICE_CONFIG.md](../fx10_driver/docs/DEVICE_CONFIG.md); this document covers
the lifecycle, buffering, failure policy and on-disk format common to both.

## Scope

| Component | Sources | Build target | Role |
|---|---|---|---|
| RAW format | `ebus_driver/src/raw_format.cpp`, `ebus_driver/include/ebus_driver/raw_format.h` | `ebus_raw` (no SDK) | Header serialization, CRC-32C |
| RAW recorder | `ebus_driver/src/raw_recorder.cpp`, `ebus_driver/include/ebus_driver/raw_recorder.h` | `ebus_raw` | Segments, events, synchronization, completion |
| Block-ID accounting | `ebus_driver/include/ebus_driver/block_ids.h` | header-only | GVSP block-ID gaps and anomalies |
| Camera engine | `ebus_driver/src/camera_driver.cpp`, `ebus_driver/include/ebus_driver/camera_driver.h` | `ebus_camera` | `EbusCameraDriver`: lifecycle, workers, failure policy |
| Go-X profile | `ebus_driver/src/gox_config.cpp` | `gox_device_config` (`gox_driver/CMakeLists.txt`) | `MakeGoxCameras`, `LoadGoxCameras` |
| FX10 profile | `ebus_driver/src/fx10_config.cpp` | `fx10_device_config` (`fx10_driver/CMakeLists.txt`) | `MakeFx10Camera`, `LoadFx10Camera` |
| Tool helpers | `ebus_driver/include/ebus_driver/tool_support.h` | header-only | Snapshot and stream orchestration |
| Tests | `ebus_driver/tests/test_raw.cpp`, `ebus_driver/tests/test_block_ids.cpp` | `ebus_raw_tests` (CTest `ebus.raw`) | doctest unit tests of the recorder and block IDs |

`main.cpp` (PhenomateDrivers) creates one `EbusCameraDriver` per enabled Go-X camera and one for
the FX10; `jai_snapshot`, `fx10_snapshot` and `fx10_reference` use the same engine. Recordings are
read by the GUI (`app/decoder/ebus/`) and by the separate offline project PhenomateDecoders.

The recorded level is the SDK buffer: the `GetAcquiredSize()` bytes the SDK delivers after GVSP
reassembly, not a packet capture. Packet headers and any SDK processing before delivery cannot be
recovered from a recording. The engine does not unpack, crop, strip padding, chunk data or status
lines, demosaic, calibrate or synthesize pixels; the camera's `PixelFormat`, ROI and binning
settings determine the information content.

## Lifecycle and ownership

`EbusCameraDriver` implements `common::IDriverApp` and is single-use: `Init` and `Run` each run
once, and `Shutdown` is idempotent and also runs from the destructor. A failed `Init` returns
`false`; the caller still calls `Shutdown`, which releases whatever was acquired.

| Phase | Step | Action |
|---|---|---|
| Init | 1 | Require `id` and `kind`; bootstrap the eBUS environment; resolve `mac` by discovery (`common::Ebus::FindCamera`), otherwise use `ip`. |
| Init | 2 | `PvDeviceGEV::Connect` with control access; profile `prepare` hook. |
| Init | 3 | Open the stream: socket receive buffer, `PvStreamGEV::Open`, packet size (configured, or negotiated with a 1476 B fallback), `SetStreamDestination`, profile `configure_stream` hook. |
| Init | 4 | Profile `configure` hook. |
| Init | 5 | Select the block-ID mode from the `GevGVSPExtendedIDMode` readback (profile fallback when the node is absent; an unrecognized value fails). |
| Init | 6 | Read `PayloadSize`; allocate and queue the SDK buffers; allocate the application pool. |
| Init | 7 | Open the recorder: camera directory, `device.json`, `events.jsonl`, `seg_00001.raw`. |
| Init | 8 | Register with the shared `common::SensorSyncHub` when enabled; event `prepared`. |
| Run | 9 | Start the writer thread, then the receiver thread, which waits on a start gate. |
| Run | 10 | Profile `before_start` hook; event `profile_start` with its result. |
| Run | 11 | Fail if `PayloadSize` changed; `StreamEnable`; `AcquisitionStart`; open the start gate; `Arm` SensorSync; event `acquisition_started`; health sample `start`. |
| Run | 12 | Monitor every 100 ms until a stop or failure (see "Failure policy"). |
| Stop | 13 | `Disarm` SensorSync; `AcquisitionStop`; let the receiver drain and join it; close the queue; join the writer after it has emptied the queue. |
| Shutdown | 14 | Health sample `stop` and trigger accounting; `stream_final_statistics`; SensorSync quality. |
| Shutdown | 15 | `StreamDisable`; `AbortQueuedBuffers`; retrieve the aborted buffers (at most 5 s); `PvStreamGEV::Close`; free the SDK buffers; destroy the profile hooks; `Disconnect`. |
| Shutdown | 16 | Compare records written with buffers admitted; event `capture_final`; close the recorder (`session_end`). |

Step 13 runs at the end of `Run`; `Shutdown` repeats it as a no-op or performs it after a partial
start. The SDK owns a `PvBuffer` only while it is queued. The receiver copies the acquired bytes
into a pooled `RawBuffer` and requeues the `PvBuffer` before it publishes the copy; an RAII guard
requeues it on every other path. The writer receives `RawBuffer` ownership through the queue,
appends it and returns the slot to the pool without touching SDK objects.

### Profile hooks

`DeviceHooks` (`ebus_driver/include/ebus_driver/camera_driver.h`) carries the device-specific
steps; their content is described in the device documents.

| Hook | Called | Go-X (`ebus_driver/src/gox_config.cpp`) | FX10 (`ebus_driver/src/fx10_config.cpp`) |
|---|---|---|---|
| `prepare` | Init, after connect | Factory user-set load; PTP enable and lock wait when `ptp.enabled` | Factory preparation; `.pvcfg` restore |
| `configure_stream` | Stream open | `network.receiver_tuning` writes | Not set |
| `configure` | Init, after stream open | Apply plan; require `GevGVSPExtendedIDMode` On and the required `PixelFormat` and `SensorDigitizationBits`; Counter0 binding | Acquisition settings; geometry; wavelengths; `Counter1_EventSource` binding |
| `metadata` | Init, once | Device report, `profile`, `legacy_output_options`, `trigger_counter` | Device metadata, `profile`, `timing`, `legacy_output_options`, `factory_preparation` |
| `before_start` | Run | Counter0 reset and baseline (`trigger_counter`) | Shutter open, or closed for a dark reference (`shutter_preparation`); missed-trigger baseline (`missed_triggers`) |
| `sample` | Health samples | Telemetry, PTP state, Counter0 checks; `triggered_frames` when every reading is valid | Temperatures, missed-trigger delta |

### Profile options

| Option | Engine default | Go-X | FX10 |
|---|---|---|---|
| `recording.directory` | none | `<output>/<id>` | `<output>` as passed by the caller |
| `recording.segment_bytes` | 2048 MiB | `output.segment_size_mb` MiB | `recording.rotation.max_mb` MiB; 0 = unlimited |
| `recording.segment_records` | 0 (no limit) | 0 | `recording.rotation.max_lines` |
| `recording.flush_bytes` | 64 MiB | `output.flush_interval_mb` MiB | `recording.flush_interval_mb` MiB |
| `sdk_buffers` | 16 | `network.buffer_count`, or automatic from the rate | `network.buffer_count`, or 16 |
| `queue_frames` | 0 (derived) | `output.queue_max_frames`, or automatic from the rate | 0 |
| `max_buffer_memory_mb` | 512 | 512 | `network.max_buffer_memory_mb` |
| `retrieve_timeout_ms` | 1000 | 1000 | `network.retrieve_timeout_ms` |
| `stall_budget_s` | 2 | 2 | `network.stall_budget_s` |
| `expected_rate_hz` | 1 | `acquisition.frame_rate_hz`, or 1 | `acquisition.frame_rate_hz` |
| `max_frames`, `max_duration_s` | 0 (unlimited) | `output.*` | `recording.*` |
| `telemetry_interval_s` | 5 | 5 | 60 |
| `statistics_interval_s` | 2.5 | `logging.stats_interval_s` | `logging.stats_interval_s` |
| `extended_block_ids` (fallback) | false | true | false |
| `local_ip` | empty (SDK chooses) | `network.local_ip` | empty |
| `expect_data` | true | false only for an external trigger without SensorSync | same rule |
| `pulse_hz` | 0 | `frame_rate_hz` for an external trigger with SensorSync, else 0 | same rule |

## Buffers, queue and workers

`socket_rx_buffer_mb` MiB is requested with `SetUserModeSocketRxBufferSize` before
`PvStreamGEV::Open`. `INVALID_PARAMETER` means the SDK capped the request at the host
`net.core.rmem_max`, as documented in the SDK's `PvStreamGEV.h`; the engine then logs a warning,
records `/proc/sys/net/core/rmem_max` and continues. After `Open` the size is read back; zero, a
value below an uncapped request, or an error other than `NOT_SUPPORTED` fails Init. The readback
may include kernel bookkeeping and is not a payload capacity; the engine never changes host
settings.

| Quantity | Rule |
|---|---|
| Slots | `floor(max_buffer_memory_mb × 2^20 / PayloadSize)`; fewer than 8 fails Init. |
| SDK buffers | `min(max(4, sdk_buffers), GetQueuedBufferMaximum() if nonzero, slots − 4)`, each `PvBuffer::Alloc(PayloadSize)`; fewer than 4 fails Init. |
| Recording queue | `min(wanted, slots − SDK buffers − 2)`; `wanted` is `queue_frames`, or `max(2, ceil(expected_rate_hz × stall_budget_s))` when it is 0; a `wanted` below 2 fails Init. |
| Application pool | Queue capacity + 2 `RawBuffer` slots, each reserving `PayloadSize` bytes. |

The budget covers payload allocations only; SDK internals and JSON metadata are outside it.
`device.json` `runtime` records the counts, `payload_buffer_bytes`, `estimated_queue_stall_seconds`
(queue capacity / `expected_rate_hz`) and whether the SDK count or the queue was clamped. Init also
rejects non-finite or out-of-range timing options, including `retrieve_timeout_ms` outside
1–10 000.

The receiver calls `RetrieveBuffer` with `retrieve_timeout_ms` and retries on timeout. After
`AcquisitionStop` it drains with a 200 ms timeout and ends after 1 s without a buffer or on
`ABORTED`; a drain longer than 5 s in total fails the session. Each retrieved buffer is classified
in this order:

| Condition | Action |
|---|---|
| Unsafe extent: `GetAcquiredSize()` > `PayloadSize`, or a null data pointer with nonzero size | No byte is read; event row `unsafe_buffer_rejected`; session fails. |
| Valid frame after `max_frames` valid frames were admitted | Event row `limit_excluded_buffer`; no failure. |
| No free pool slot, or the queue is full | Event row `queue_rejected_buffer`; session fails; admitted buffers still drain. |
| Otherwise | Bytes copied, `PvBuffer` requeued, `RawBuffer` queued for the writer. |

The writer pops until the queue is closed and empty. After its first append error it stops writing
and counts the failed buffer and every later one as `write_unconfirmed`.

`BlockIds` (`ebus_driver/include/ebus_driver/block_ids.h`) takes its mode from configuration,
never from observed IDs. In 16-bit mode valid IDs cycle through 1–65 535, the forward distance from
the expected ID is the gap, and a distance above 32 767 is an anomaly. In 64-bit mode an ID not
greater than the previous one is an anomaly and the gap is the difference minus one. ID 0, and IDs
above 65 535 in 16-bit mode, are anomalies; an anomaly does not move the baseline.

## Failure policy

`Fail` latches the driver failure (`IDriverApp::RequestFailure`), requests a stop and keeps the
first message as the error; there is no retry or reconnect. `Stop` ends the session without
failure. In PhenomateDrivers one failed driver stops the rig (`common/src/session_runner.cpp`).

| Condition | Recorded as | Result |
|---|---|---|
| SDK operation error on a buffer | Record, `operation_ok: false` | Fails |
| Block-ID gap | Record, `gap_before` > 0 (the frame itself may be valid) | Fails |
| Block-ID anomaly | Record, `block_id_anomaly: true` | Fails |
| Image payload incomplete or its extent unproven | Record, `valid_frame: false` | Fails |
| Non-image payload | Record, `image: null`; counter `non_image_payloads` | No failure by itself |
| Unsafe extent; pool or queue exhausted | Event row (see above) | Fails |
| `QueueBuffer` or `RetrieveBuffer` error (other than timeout, or `ABORTED` after stop) | Log | Fails |
| `BlocksDropped` stream counter increases | Log | Fails |
| Control link disconnected | Log | Fails |
| Profile health sample reports a failure | `device_health.failure` | Fails |
| SensorSync error, log idle > 15 s, participants not armed within 30 s | Log | Fails |
| Shared SensorSync session ended | Reason | Stops |
| `max_duration_s` (from just before `AcquisitionStart`) or `max_frames` reached | Reason | Stops |
| RAW append error | `write_unconfirmed` for it and every later buffer | Fails |
| Event write error | Log | Fails |
| Receive drain > 5 s; `AcquisitionStop` failed or link down at stop | Counter, log | Fails |
| Completed SDK buffer found at teardown | Event `undrained_sdk_buffer` | Fails |
| No valid frame although `expect_data` | Log | Fails |
| Trigger counter differs from valid frames + gaps (bounded external trigger) | `trigger_counter_accounting` | Fails |
| SensorSync edge accounting incomplete | `sensor_sync_quality` | Fails |
| Records written ≠ buffers admitted, or `write_unconfirmed` > 0 | Log | Fails |
| `StreamDisable`, `AbortQueuedBuffers`, stream close or `Disconnect` error | Log | Fails |

`MicrosSinceLastData()` returns the time since the last valid buffer; before the first one it
counts from `AcquisitionStart`, or from the SensorSync session start. It returns no value when
`expect_data` is false, before acquisition, or while a registered camera waits for the SensorSync
session. The rig's no-data guard and the tools' 10 s starvation check use it.

## Statistics and counters

Every `statistics_interval_s` (0 disables) the engine logs, under module `GoX` or `FX10`:

```text
[Statistics] [<id>] up=<s>s rate=<r> Hz  fps=<f>  frames=<n> buffers=<m> lost=<g> queue=<q>/<cap>
```

| Field | Meaning |
|---|---|
| `up` | Whole seconds since the `AcquisitionStart` command |
| `rate` | Valid frames observed per second over the interval |
| `fps` | Valid frames written per second over the interval; read by `app/services/driver_stats.py` (contract in `tools/check_contracts.py`) |
| `frames`, `buffers` | Valid frames written; records written |
| `lost` | Sum of block-ID gaps |
| `queue` | Current queue depth / capacity |

`capture_final.counters` and `FinalStatistics().counters` hold:

| Counter | Meaning |
|---|---|
| `buffers_retrieved`, `buffers_admitted` | Buffers returned by `RetrieveBuffer`; buffers queued for the writer |
| `valid_frames_observed`, `valid_frames_admitted` | Valid frames retrieved (including limit exclusions); valid frames queued |
| `operation_errors`, `non_image_payloads`, `invalid_images` | Buffers with an SDK operation error, a non-image payload, an incomplete image |
| `frames_lost_gap`, `block_id_gap_events`, `block_id_anomalies` | Missing block IDs; gap occurrences; anomalous IDs |
| `recording_queue_drops`, `unsafe_buffers`, `frames_limit_excluded` | Rejected or excluded buffers (event rows) |
| `write_unconfirmed`, `requeue_errors` | Buffers not confirmed written; failed requeues |
| `retrieve_timeouts`, `aborted_buffers` | `RetrieveBuffer` timeouts; buffers with an `ABORTED` operation result |
| `stream_blocks_dropped`, `drain_budget_exhausted` | Last SDK `BlocksDropped` value; drains that exceeded 5 s |

`FinalStatistics()` returns `schema_version` 1, `driver` `unified-ebus`, `kind`, `camera_id`,
`initialized`, `acquisition_started`, `shutdown_complete`, `clean`, `failed`, `reason`, `error`,
`counters`, `frames_written`, `records_written`, `trigger_counter`, `sensor_sync` and `recording`
(the recorder summary). PhenomateDrivers stores it in `drivers.json`; the tools derive their exit
status from it.

## Output files

Each camera writes one directory, which must not exist before Init.

| File | Content |
|---|---|
| `device.json` | Device metadata, 2-space indented JSON, written once during Init |
| `events.jsonl` | One compact JSON object per line; `fdatasync` after every line |
| `seg_NNNNN.raw` | RAW segments, `seg_%05llu.raw`, numbered from 1 |
| `device.json.part` | Present only if publication of `device.json` was interrupted |

## RAW binary layout

The layout is defined in `ebus_driver/src/raw_format.cpp` and `ebus_driver/src/raw_recorder.cpp`.
All integers are little-endian and serialized byte by byte; no C++ structure is written and there
is no alignment padding. A segment is a file header followed by zero or more contiguous records; a
record is never split across segments.

CRC-32C (Castagnoli) uses the reflected polynomial `0x82F63B78`, initial value `0xFFFFFFFF` and a
final XOR with `0xFFFFFFFF`; `Crc32c("123456789")` is `0xE3069283`. SSE4.2 instructions are used
when the CPU supports them.

File header (`kFileHeaderSize` = 64, `MakeFileHeader`; Python `struct` format `<8sHHIQQ28sI`):

| Offset | Size | Type | Field | Value |
|---|---|---|---|---|
| 0 | 8 | bytes | magic | `EBUSRAW\0` |
| 8 | 2 | u16 | version | 1 (`kVersion`) |
| 10 | 2 | u16 | header_size | 64 |
| 12 | 4 | u32 | flags | 0 |
| 16 | 8 | u64 | created_realtime_ns | `CLOCK_REALTIME` at segment creation |
| 24 | 8 | u64 | segment_index | 1-based; equals the file-name number |
| 32 | 28 | bytes | reserved | 0 |
| 60 | 4 | u32 | header_crc | CRC-32C of bytes 0–59 |

Record = 48-byte prefix + `metadata_size` bytes of UTF-8 JSON + `payload_size` bytes of payload.
Record prefix (`kRecordHeaderSize` = 48, `MakeRecordHeader`; Python format `<4sIIIQQIIII`):

| Offset | Size | Type | Field | Value |
|---|---|---|---|---|
| 0 | 4 | bytes | magic | `EBFR` |
| 4 | 4 | u32 | prefix_size | 48 |
| 8 | 4 | u32 | metadata_size | ≥ 2 (a JSON object); at most 1 MiB (`kMaxMetadataSize` = 1 048 576) |
| 12 | 4 | u32 | flags | 0 |
| 16 | 8 | u64 | payload_size | Equals metadata `acquired_size` |
| 24 | 8 | u64 | sequence | 0-based, contiguous across the segments of one camera |
| 32 | 4 | u32 | metadata_crc | CRC-32C of the metadata bytes |
| 36 | 4 | u32 | payload_crc | CRC-32C of the payload bytes |
| 40 | 4 | u32 | prefix_crc | CRC-32C of prefix bytes 0–39 |
| 44 | 4 | u32 | reserved | 0 |

Before an append, the recorder opens a new segment if the current one already holds a record and
either `segment_records` (when nonzero) is reached or the record would extend the file beyond
`segment_bytes`. The limit counts the file header and complete records; a record larger than the
limit occupies a segment alone, so such a segment exceeds `segment_bytes`. Metadata larger than
1 MiB fails the append.

### Record metadata

Records written to segments have `record_kind` `buffer`. The metadata is serialized compactly;
readers must not depend on key order.

| Key | Type | Content |
|---|---|---|
| `record_kind` | string | `buffer` |
| `block_id` | u64 | `PvBuffer::GetBlockID()` |
| `device_timestamp_raw` | u64 | `PvBuffer::GetTimestamp()`, unscaled; unit and epoch unverified |
| `host_realtime_ns`, `host_monotonic_ns` | u64 | `CLOCK_REALTIME` and `CLOCK_MONOTONIC` sampled right after `RetrieveBuffer`; reception time, not exposure time |
| `acquired_size` | u64 | `GetAcquiredSize()`; equals the prefix `payload_size` |
| `payload_type` | u32 | `PvPayloadType` value |
| `operation_result`, `operation_ok`, `operation_name` | u32, bool, string | `PvResult` code of the buffer operation |
| `chunk_count` | u32 | `GetChunkCount()` |
| `chunks` | array or null | `[{"id": <chunk ID>, "data": [<byte>, ...]}]` read before requeue; the same bytes remain in the payload tail; null when the extent is unsafe, the operation failed or a listed chunk cannot be read |
| `valid_frame` | bool | See below |
| `block_id_anomaly` | bool | Anomaly from `BlockIds` |
| `gap_before` | u64 | Block IDs missing before this buffer |
| `image` | object or null | Null for a non-image payload or when the SDK provides no image |

`image` contains `pixel_format` (PFNC code), `width`, `height`, `offset_x`, `offset_y` (ROI offsets
in pixels, not byte offsets), `padding_x`, `padding_y`, `image_size`, `effective_image_size`,
`payload_offset`, `partial_line_missing`, `full_line_missing` and `data_overrun`. `payload_offset`
is the byte offset of the SDK image pointer within the payload; it is null unless the image lies
within the acquired bytes, and readers must not guess it. `valid_frame` is true only when the
operation succeeded, the extent is safe, the block ID is not anomalous, and the image has a known
`payload_offset`, nonzero dimensions and sizes, `effective_image_size` ≤ `image_size` and no
missing-line or overrun flag.

## Device metadata (`device.json`)

`device.json` is written once, during Init after configuration and allocation, and never rewritten.
The profile `metadata` hook supplies the device-specific content (see the device documents); the
engine then sets the keys below, replacing any profile value.

| Key | Content |
|---|---|
| `format` | `ebus-device-eBUS_SDK_v<version>` |
| `recording_format` | `ebus-raw-eBUS_SDK_v<version>` |
| `kind`, `id` | `gox` or `fx10`; the camera id (`fx10` for the FX10) |
| `sdk` | `name` (`Pleora eBUS`), `header_version`, `header_version_source`, `runtime_version` (always null), `runtime_version_status` |
| `runtime` | Socket buffer request, SDK result, `rmem_max` snapshot and readback; stream local IP and port; `packet_size` (`GevSCPSPacketSize` readback after configuration); `payload_capacity_bytes`; buffer counts, clamps and budget scope; `queue_full_policy`; `block_id_mode`, `block_id_mode_source`, `extended_block_ids_readback` |
| `recording_contract` | Text statements of payload, chunk, timestamp, processing and limit semantics; `device_timestamp_unit` and `device_timestamp_epoch` are `unverified` |
| `sensor_sync` | `enabled`, `channel`, `pulse_rate_hz`, `observations_file`, `association_verified` (always false) |

`<version>` is `NVERSION_STRING` from the SDK's `PvVersion.h` at build time (`SdkHeaderVersion()`,
e.g. `6.5.1.6797`) and equals `sdk.header_version`. It identifies the SDK headers, not the runtime
library or the PhenomateDrivers version; the runtime SDK version is not queried. The binary layout
version (file header `version`) is independent of it.

## Events (`events.jsonl`)

Events emitted by the engine carry `camera_id`, `host_realtime_ns` and `host_monotonic_ns`.
`session_start`, `segment_closed` and `session_end` are emitted by the recorder and carry no
`camera_id`; `segment_closed` carries no time. Events raised before the recorder opens appear only
in the log.

| Event | Emitted | Fields |
|---|---|---|
| `session_start` | Recorder open | `format` |
| `prepared` | End of Init | `acquisition_started: false` |
| `initialization_interrupted` | Stop during Init, after the recorder opened | `detail` |
| `profile_start` | Run, after `before_start` | `actions` (profile results) |
| `acquisition_started` | After `AcquisitionStart` was acknowledged | `command_begin_monotonic_ns`, `command_ack_monotonic_ns` |
| `device_health` | Start, every `telemetry_interval_s`, stop | `phase` (`start`, `capture`, `stop`), `values`, `failure` (empty when healthy) |
| (no `event` key) | Receive worker | Full record metadata with `record_kind` `unsafe_buffer_rejected` (plus `payload_capacity_bytes`, `reason`), `limit_excluded_buffer` or `queue_rejected_buffer` |
| `segment_closed` | Rotation and close | `file`, `bytes` (including the header), `records`, `closed_clean` |
| `trigger_counter_accounting` | Stop health sample | `accounting`: `status` (`unavailable`, `counts_match`, `count_mismatch`, `count_overflow`), `triggered_frames`, `source_bounded`, `valid_observed_frames`, `gap_missing_frames`, `emitted_frames`, `counts_match`, `association_verified` |
| `stream_final_statistics` | Shutdown | `values`: every `PvStream` parameter as a string, null if unreadable |
| `sensor_sync_quality` | Shutdown, when registered with SensorSync | `quality` (see below) |
| `undrained_sdk_buffer` | Stream teardown | `block_id`, `operation_name` |
| `capture_final` | Shutdown, before recorder close | `counters`, `reason`, `acquisition_started`, `failed` |
| `session_end` | Recorder close | Recorder summary, `complete`, `reason`, `host_realtime_ns` |

Trigger accounting is bounded when the SensorSync session started and is healthy and `pulse_hz` > 0;
then the profile's `triggered_frames` must equal valid frames observed plus block-ID gaps. FX10
samples never provide `triggered_frames`, so its status is `unavailable`. `sensor_sync_quality`
holds the result of `common::InspectSensorSync` plus frame counts; for freerun cameras
(`pulse_hz` = 0) it checks log integrity only, with `status` `observation_window_only` and
`counts_reconciled` null, because the capture and log windows differ. `status` `not_started` means
the session never started, which fails a camera that acquired.

The recorder summary in `session_end` contains `format`, `directory`, `records_written`,
`frames_written` and `valid_frames_written` (equal values), `payload_bytes_written`,
`segments_created`, `failed` and `count_semantics`.

## Completion and recovery

| Mechanism | Behavior |
|---|---|
| Directory and files | `Open` refuses an existing camera directory; segments and `events.jsonl` use `O_EXCL`; `device.json` is written as `device.json.part`, synchronized, linked to its final name and unlinked. |
| Segment creation | Header written, `fdatasync`, directory `fsync`. The parent directory is synchronized once after the first segment. |
| Records | `fdatasync` whenever `flush_bytes` of unsynchronized data accumulate (0 disables), and when a segment closes. A written count means `write()` returned for the whole record, not that it is durable. |
| Append error | The segment is truncated to the last complete record; if truncation fails, a torn tail remains, delimited by the prefix lengths and CRCs. The recorder latches the failure and rejects later appends. |
| Event write error | The partial line is truncated and synchronized; if that fails, the event stream is closed and no further event, including `session_end`, is written. |
| Close | Synchronize and close the last segment (`segment_closed`), `fsync` the directory, write `session_end`, close `events.jsonl`. |

`session_end.complete` is true only when Init and `AcquisitionStart` succeeded, the driver has not
failed and the recorder has not latched a failure. All segment and directory synchronization
precedes `session_end`; closing `events.jsonl` follows it. A close error after a complete
`session_end` is therefore visible only in the driver result (`drivers.json`, tool exit status).

After a crash or power loss a directory can hold `device.json.part`, an `events.jsonl` without
`session_end`, and a last segment that lost data written after its latest `fdatasync` or ends in a
torn record. Only the CRC-valid record prefix is recoverable. This repository provides no recovery
or export tool; offline conversion belongs to PhenomateDecoders.

## Tools support

`ebus_driver/include/ebus_driver/tool_support.h` provides the shared orchestration of
`jai_snapshot` and `fx10_snapshot` (`RunSnapshot`, `RunStream`); `fx10_reference` uses its signal
flag, `TimestampedPath` and `Count` with its own `EbusCameraDriver` per phase. Tool options and
modes are described in the device documents.

| Item | Behavior |
|---|---|
| Signals | SIGINT and SIGTERM set `ebus_driver::tool::signal`. |
| Stop predicate | Signal, or no valid data for 10 s (`MicrosSinceLastData()` ≥ 10 000 000 µs). |
| Sequence | `Init`, `Run` if `Init` succeeded, `Shutdown`, then `FinalStatistics()`. |
| `TimestampedPath` | `<parent>/<stem>_<YYYYmmddTHHMMSSZ>_<nanoseconds within the second>`, absolute. |
| Stream mode | No frame target; a signal after a successful `Init` ends the stream normally. |

The last stdout line is `SNAPSHOT: OK <camera directory>` (exit 0) or
`SNAPSHOT: FAIL <code> <reason>`:

| Code | Condition |
|---|---|
| 2 | Invalid arguments or configuration (raised by the tool itself) |
| 3 | `Init` failed |
| 5 | Starvation, driver failure, exception, or fewer valid frames written than requested |
| 7 | No valid frame written |
| 130 | Signal during a snapshot, or during a stream before `Init` succeeded |

For the focus-assist streams, `jai_snapshot --stream-s` writes one record per segment
(`segment_records` = 1) and `fx10_snapshot --stream-s` rotates by line count. The GUI keeps the
three newest segments while streaming and deletes all segments when the stream ends
(`app/services/focus_stream.py`), so a stream directory is never a complete session.

## Compatibility

The GUI reader (`app/decoder/ebus/recording.py`, `pixels.py`, `pfnc.py`) applies these rules.
PhenomateDecoders defines its own acceptance rules.

| Check | Accepted |
|---|---|
| `device.json` | `sdk.header_version` of four dot-separated integers; `format` = `ebus-device-eBUS_SDK_v` + that version; `recording_format` = `ebus-raw-eBUS_SDK_v` + that version; `kind` `gox` or `fx10`; string `id`. Unsuffixed or `-v1` identities and the former `jai-raw-seg` and FX10 ENVI recordings are rejected. |
| File header | Magic, `version` 1, `header_size` 64, `flags` 0, zero reserved bytes, `segment_index` ≥ 1, header CRC. |
| Record prefix | Magic, `prefix_size` 48, `flags` 0, `reserved` 0, `metadata_size` > 0, prefix CRC, metadata CRC. |
| Metadata | `record_kind` `buffer`; integer `block_id`, `device_timestamp_raw`, `host_realtime_ns`, `acquired_size`; `acquired_size` = `payload_size`; `image` key present; `valid_frame` consistent with `operation_ok`, `block_id_anomaly` and the image flags. Other keys, including `chunks`, are ignored. |
| Payload CRC | Checked only when `read_payload(..., verify_crc=True)` is requested; previews do not request it by default. |
| Finalized session | Every `events.jsonl` line newline-terminated; last `session_end` with `complete: true` and the device's `recording_format`; segments numbered 1..n without gaps; sequence contiguous from 0; `records_written`, `valid_frames_written`, `payload_bytes_written` and `segments_created` equal to the scanned values; a torn tail is rejected. |
| Live reads | `newest_valid_record` and `latest_records` scan the newest two or three segments, skip segments without a complete record and ignore a torn trailing record. |
| Pixel decoding | Valid frames with a known `payload_offset`; PFNC Mono8, Mono10, Mono12, Mono10Packed, Mono12Packed, BayerRG8 and BayerRG12Packed; `effective_image_size` = packed row bytes × `height`, `image_size` = (row bytes + `padding_x`) × `height` + `padding_y`, and the image slice within the payload. Other formats are recorded but not decoded. |

Adding metadata keys keeps these readers compatible; removing or retyping a checked key, or changing
the binary layout without a new header `version`, does not.

## Limitations

Not verified on hardware, or not established by the source code:

- The SDK semantics of `GetImageSize`, `GetEffectiveImageSize`, `GetPaddingX` and `GetPaddingY`
  that the GUI layout check assumes, and chunk extraction (Go-X chunk mode stays off).
- The unit and epoch of `device_timestamp_raw`; any association of host times with exposure.
- The `NVERSION_STRING` of the installed 6.5.1 headers; the reader requires four numeric fields.
- Socket buffer capping on the target host; the handling follows the SDK header documentation.
- Sustained throughput, queue headroom against disk stalls, and the 1 s and 5 s drain bounds.
- Durability of `fdatasync` and `fsync` on the target file system, and recovery after a real crash.
