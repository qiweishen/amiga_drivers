"""Bounded, SDK-free ebus-raw reader. No acquisition or write side effects.

Record metadata is self-contained. Live reads stop at a torn trailing record;
finalized snapshot/reference reads reject it. Scanning seeks over payloads, so only selected images
need to be allocated. A successful preview is not a full payload CRC audit.
"""

from __future__ import annotations

import json
import os
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

FILE_MAGIC = b"EBUSRAW\0"
RECORD_MAGIC = b"EBFR"
FILE_HEADER_SIZE = 64
RECORD_HEADER_SIZE = 48
FILE_HEADER_FMT = "<8sHHIQQ28sI"
RECORD_HEADER_FMT = "<4sIIIQQIIII"
MAX_METADATA_BYTES = 1024 * 1024
MAX_PAYLOAD_BYTES = 128 * 1024 * 1024  # allocation budget, not a raw format limit
MAX_PREVIEW_RECORDS = 4096
MAX_PREVIEW_METADATA_BYTES = 8 * 1024 * 1024
MAX_PREVIEW_SCAN_RECORDS = 100000
MAX_PREVIEW_SCAN_METADATA_BYTES = 64 * 1024 * 1024
_UINT64_MAX = (1 << 64) - 1
_SDK_VERSION = re.compile(r"[0-9]+(?:\.[0-9]+){3}")


def _crc_table():
    table = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        table.append(value)
    return tuple(table)


_CRC_TABLE = _crc_table()


def crc32c(data: bytes, seed: int = 0) -> int:
    value = (~seed) & 0xFFFFFFFF
    for byte in data:
        value = _CRC_TABLE[(value ^ byte) & 255] ^ (value >> 8)
    return (~value) & 0xFFFFFFFF


def _uint(value, name, maximum=_UINT64_MAX):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f"Invalid ebus-raw {name}")
    return value


def _boolean(value, name):
    if type(value) is not bool:
        raise ValueError(f"Invalid ebus-raw {name}")
    return value


def _json(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    def reject_constant(value):
        raise ValueError(f"Invalid JSON {value}")

    return json.loads(raw.decode("utf-8"), object_pairs_hook=unique, parse_constant=reject_constant)


def metadata_document(path: Path) -> dict:
    with path.open("rb") as source:
        raw = source.read(MAX_METADATA_BYTES + 1)
    if len(raw) > MAX_METADATA_BYTES:
        raise ValueError(f"Metadata exceeds limit: {path}")
    result = _json(raw)
    if not isinstance(result, dict):
        raise ValueError(f"Metadata must be an object: {path}")
    return result


def _supported_device_metadata(device: dict) -> bool:
    sdk = device.get("sdk")
    version = sdk.get("header_version") if isinstance(sdk, dict) else None
    if not isinstance(version, str) or _SDK_VERSION.fullmatch(version) is None:
        return False
    # This identifies the producing SDK; the binary header independently declares its layout.
    suffix = f"-eBUS_SDK_v{version}"
    return (device.get("format") == "ebus-device" + suffix
            and device.get("recording_format") == "ebus-raw" + suffix
            and device.get("kind") in ("gox", "fx10") and isinstance(device.get("id"), str))


def device_metadata(directory: Path) -> dict:
    device = metadata_document(directory / "device.json")
    if not _supported_device_metadata(device):
        raise ValueError("Unsupported eBUS device metadata: expected matching SDK-suffixed formats "
                         "and sdk.header_version")
    return device


def is_segment(path: Path) -> bool:
    with Path(path).open("rb") as source:
        return source.read(8) == FILE_MAGIC


def segment_paths(directory: Path) -> list[Path]:
    paths = list(Path(directory).glob("seg_*.raw"))
    return sorted(paths, key=lambda path: int(path.stem.removeprefix("seg_")))


def is_session(directory: Path) -> bool:
    path = Path(directory) / "device.json"
    if path.is_file():
        return _supported_device_metadata(metadata_document(path))
    segments = segment_paths(directory)
    return bool(segments and is_segment(segments[0]))


@dataclass(frozen=True)
class FileHeader:
    created_realtime_ns: int
    segment_index: int


def read_file_header(source) -> FileHeader:
    source.seek(0)
    raw = source.read(FILE_HEADER_SIZE)
    if len(raw) != FILE_HEADER_SIZE:
        raise ValueError("Incomplete eBUS file header")
    magic, version, size, flags, created, index, reserved, checksum = struct.unpack(FILE_HEADER_FMT, raw)
    if (magic != FILE_MAGIC or version != 1 or size != FILE_HEADER_SIZE or flags != 0
            or reserved != bytes(28) or index < 1):
        raise ValueError("Unsupported eBUS file header")
    if crc32c(raw[:60]) != checksum:
        raise ValueError("eBUS file header CRC-32C mismatch")
    return FileHeader(created, index)


def _validate_metadata(meta, payload_size):
    if not isinstance(meta, dict) or meta.get("record_kind") != "buffer":
        raise ValueError("Unsupported eBUS record metadata")
    for name in ("block_id", "device_timestamp_raw", "host_realtime_ns", "host_monotonic_ns",
                 "acquired_size", "payload_type", "operation_result", "chunk_count", "gap_before"):
        _uint(meta.get(name), name)
    if meta["acquired_size"] != payload_size:
        raise ValueError("Acquired size disagrees with the raw record prefix")
    for name in ("operation_ok", "valid_frame", "block_id_anomaly"):
        _boolean(meta.get(name), name)
    if not isinstance(meta.get("operation_name"), str):
        raise ValueError("Invalid eBUS operation name")
    if "image" not in meta:
        raise ValueError("Missing eBUS image field (use null for a non-image buffer)")
    image = meta["image"]
    if image is not None:
        if not isinstance(image, dict):
            raise ValueError("Invalid eBUS image metadata")
        for name in ("pixel_format", "width", "height", "offset_x", "offset_y", "padding_x",
                     "padding_y", "image_size", "effective_image_size"):
            _uint(image.get(name), "image." + name)
        if image.get("payload_offset") is not None:
            _uint(image["payload_offset"], "image.payload_offset")
        for name in ("partial_line_missing", "full_line_missing", "data_overrun"):
            _boolean(image.get(name), "image." + name)
    if meta["valid_frame"] and (not meta["operation_ok"] or meta["block_id_anomaly"] or image is None
                                or any(
                image[name] for name in ("partial_line_missing", "full_line_missing", "data_overrun"))):
        raise ValueError("valid_frame contradicts the recorded receive result")


@dataclass(frozen=True)
class Record:
    path: Path
    offset: int
    sequence: int
    metadata: dict
    metadata_size: int
    payload_size: int
    payload_crc32c: int

    @property
    def payload_offset(self):
        return self.offset + RECORD_HEADER_SIZE + self.metadata_size

    @property
    def end(self):
        return self.payload_offset + self.payload_size

    @property
    def valid_frame(self):
        return self.metadata["valid_frame"]

    # Short field names used by GOX snapshot and live-preview diagnostics.
    @property
    def seq(self):
        return self.sequence

    @property
    def bid(self):
        return self.metadata["block_id"]

    @property
    def dts(self):
        return self.metadata["device_timestamp_raw"]

    @property
    def hrt(self):
        return self.metadata["host_realtime_ns"]

    @property
    def pf(self):
        return (self.metadata["image"] or {}).get("pixel_format", 0)

    @property
    def w(self):
        return (self.metadata["image"] or {}).get("width", 0)

    @property
    def h(self):
        return (self.metadata["image"] or {}).get("height", 0)


def _record_at(source, path, offset, size, live):
    source.seek(offset)
    raw = source.read(RECORD_HEADER_SIZE)
    if len(raw) != RECORD_HEADER_SIZE:
        if live:
            return None
        raise ValueError(f"Incomplete eBUS record prefix at {path}:{offset}")
    magic, prefix_size, metadata_size, flags, payload_size, seq, mcrc, pcrc, hcrc, reserved = struct.unpack(
        RECORD_HEADER_FMT, raw)
    if (magic != RECORD_MAGIC or prefix_size != RECORD_HEADER_SIZE or flags != 0 or reserved != 0
            or not 0 < metadata_size <= MAX_METADATA_BYTES):
        raise ValueError(f"Invalid eBUS record prefix at {path}:{offset}")
    if crc32c(raw[:40]) != hcrc:
        raise ValueError(f"eBUS record prefix CRC-32C mismatch at {path}:{offset}")
    end = offset + RECORD_HEADER_SIZE + metadata_size + payload_size
    if end > size:
        if live:
            return None
        raise ValueError(f"Incomplete eBUS metadata/payload at {path}:{offset}")
    metadata_raw = source.read(metadata_size)
    if crc32c(metadata_raw) != mcrc:
        raise ValueError(f"eBUS metadata CRC-32C mismatch at {path}:{offset}")
    meta = _json(metadata_raw)
    _validate_metadata(meta, payload_size)
    return Record(Path(path), offset, seq, meta, metadata_size, payload_size, pcrc)


def iter_records(path: Path, *, live: bool = False) -> Iterator[Record]:
    """Scan a fixed-size snapshot, validating metadata while seeking over payloads."""
    with Path(path).open("rb") as source:
        size = os.fstat(source.fileno()).st_size
        read_file_header(source)
        offset = FILE_HEADER_SIZE
        previous = None
        while offset < size:
            record = _record_at(source, path, offset, size, live)
            if record is None:
                return
            if previous is not None and record.sequence != previous + 1:
                raise ValueError("Missing, duplicate or reversed eBUS record sequence")
            yield record
            previous, offset = record.sequence, record.end


def read_payload(record: Record, *, verify_crc: bool = False) -> bytes:
    if record.payload_size > MAX_PAYLOAD_BYTES:
        raise ValueError("eBUS payload exceeds the image allocation budget")
    with record.path.open("rb") as source:
        source.seek(record.payload_offset)
        payload = source.read(record.payload_size)
    if len(payload) != record.payload_size:
        raise ValueError("eBUS payload became incomplete while reading")
    if verify_crc and crc32c(payload) != record.payload_crc32c:
        raise ValueError(f"eBUS payload CRC-32C mismatch for record {record.sequence}")
    return payload


def session_end(directory: Path) -> dict:
    recording_format = device_metadata(Path(directory))["recording_format"]
    result = None
    with (Path(directory) / "events.jsonl").open("rb") as source:
        for raw in iter(lambda: source.readline(MAX_METADATA_BYTES + 1), b""):
            if len(raw) > MAX_METADATA_BYTES or not raw.endswith(b"\n"):
                raise ValueError("Incomplete or oversized eBUS event")
            event = _json(raw)
            if not isinstance(event, dict):
                raise ValueError("Invalid eBUS event")
            if event.get("event") == "session_end":
                result = event
    if result is None or result.get("complete") is not True:
        raise ValueError("eBUS recording has no complete session_end event")
    if result.get("format") != recording_format:
        raise ValueError("eBUS session_end format disagrees with device metadata")
    for field in ("records_written", "valid_frames_written", "payload_bytes_written", "segments_created"):
        _uint(result.get(field), field)
    return result


def session_records(directory: Path) -> Iterator[Record]:
    """Read a finalized snapshot/reference and verify its completion counters."""
    ending = session_end(directory)
    segments = segment_paths(directory)
    if not segments:
        raise ValueError("No eBUS raw segments")
    count = valid = payload_bytes = 0
    for index, path in enumerate(segments, 1):
        with path.open("rb") as source:
            if read_file_header(source).segment_index != index:
                raise ValueError("Missing or reordered eBUS segment")
        for record in iter_records(path):
            if record.sequence != count:
                raise ValueError("Missing, duplicate or reversed eBUS session record")
            count += 1
            valid += int(record.valid_frame)
            payload_bytes += record.payload_size
            yield record
    if (ending["records_written"] != count or ending["valid_frames_written"] != valid
            or ending["payload_bytes_written"] != payload_bytes or ending["segments_created"] != len(segments)):
        raise ValueError("eBUS session_end counters disagree with raw records")


def first_preview_frame(directory: Path, *, verify_crc: bool = False):
    skipped = False
    selected = None
    for count, record in enumerate(session_records(directory), 1):
        if count > 4096:
            raise ValueError("Snapshot exceeds the metadata scan budget")
        if selected is None:
            if record.valid_frame:
                selected = record
            else:
                skipped = True
    if selected is None:
        raise ValueError("No valid recorded image")
    return selected, read_payload(selected, verify_crc=verify_crc), skipped


def latest_records(directory: Path, limit: int) -> list[Record]:
    """Bounded payload selection; scan only the newest segment and its predecessor."""
    from collections import deque
    selected = deque()
    limit = min(max(1, limit), MAX_PREVIEW_RECORDS)
    metadata_bytes = 0
    scanned = scanned_bytes = 0
    for path in segment_paths(directory)[-2:]:
        for record in iter_records(path, live=True):
            scanned += 1
            scanned_bytes += record.metadata_size
            if scanned > MAX_PREVIEW_SCAN_RECORDS or scanned_bytes > MAX_PREVIEW_SCAN_METADATA_BYTES:
                raise ValueError("Recent raw segments exceed the interactive preview metadata scan budget")
            if record.valid_frame:
                selected.append(record)
                metadata_bytes += record.metadata_size
                while len(selected) > limit or metadata_bytes > MAX_PREVIEW_METADATA_BYTES:
                    metadata_bytes -= selected.popleft().metadata_size
    return list(selected)
