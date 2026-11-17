"""SDK-free ebus-raw reader. No acquisition or write side effects.

Record metadata is self-contained. Live reads stop at a torn trailing record;
finalized snapshot/reference reads reject it. Scanning seeks over payloads, so only
selected images are read. A preview without ``verify_crc`` is not a payload CRC audit.
"""

from __future__ import annotations

import json
import os
import re
import struct
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

FILE_MAGIC = b"EBUSRAW\0"
RECORD_MAGIC = b"EBFR"
FILE_HEADER_SIZE = 64
RECORD_HEADER_SIZE = 48
FILE_HEADER_FMT = "<8sHHIQQ28sI"
RECORD_HEADER_FMT = "<4sIIIQQIIII"
_SDK_VERSION = re.compile(r"[0-9]+(?:\.[0-9]+){3}")
_IMAGE_FLAGS = ("partial_line_missing", "full_line_missing", "data_overrun")


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


def _uint(value, name):
    if type(value) is not int or value < 0:
        raise ValueError(f"Invalid ebus-raw {name}")
    return value


def metadata_document(path: Path) -> dict:
    result = json.loads(Path(path).read_bytes())
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
    device = metadata_document(Path(directory) / "device.json")
    if not _supported_device_metadata(device):
        raise ValueError("Unsupported eBUS device metadata: expected matching SDK-suffixed formats "
                         "and sdk.header_version")
    return device


def is_session(directory: Path) -> bool:
    path = Path(directory) / "device.json"
    return path.is_file() and _supported_device_metadata(metadata_document(path))


def segment_paths(directory: Path) -> list[Path]:
    paths = list(Path(directory).glob("seg_*.raw"))
    return sorted(paths, key=lambda path: int(path.stem.removeprefix("seg_")))


def segment_index(source) -> int:
    """Validate a segment's file header and return its 1-based index."""
    source.seek(0)
    raw = source.read(FILE_HEADER_SIZE)
    if len(raw) != FILE_HEADER_SIZE:
        raise ValueError("Incomplete eBUS file header")
    magic, version, size, flags, _created, index, reserved, checksum = struct.unpack(FILE_HEADER_FMT, raw)
    if (magic != FILE_MAGIC or version != 1 or size != FILE_HEADER_SIZE or flags != 0
            or reserved != bytes(28) or index < 1):
        raise ValueError("Unsupported eBUS file header")
    if crc32c(raw[:60]) != checksum:
        raise ValueError("eBUS file header CRC-32C mismatch")
    return index


def _validate_metadata(meta, payload_size):
    """Check the fields the readers use; ``valid_frame`` must agree with the receive result."""
    if not isinstance(meta, dict) or meta.get("record_kind") != "buffer":
        raise ValueError("Unsupported eBUS record metadata")
    for name in ("block_id", "device_timestamp_raw", "host_realtime_ns", "acquired_size"):
        _uint(meta.get(name), name)
    if meta["acquired_size"] != payload_size:
        raise ValueError("Acquired size disagrees with the raw record prefix")
    if "image" not in meta:
        raise ValueError("Missing eBUS image field (use null for a non-image buffer)")
    image = meta["image"]
    if image is not None:
        if not isinstance(image, dict):
            raise ValueError("Invalid eBUS image metadata")
        for name in ("pixel_format", "width", "height", "padding_x", "padding_y", "image_size",
                     "effective_image_size"):
            _uint(image.get(name), "image." + name)
        if image.get("payload_offset") is not None:
            _uint(image["payload_offset"], "image.payload_offset")
    valid = meta.get("valid_frame")
    if type(valid) is not bool:
        raise ValueError("Invalid ebus-raw valid_frame")
    if valid and (meta.get("operation_ok") is not True or meta.get("block_id_anomaly") is not False
                  or image is None or any(image.get(name) is not False for name in _IMAGE_FLAGS)):
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

    # Short field names used by the GOX snapshot, live and focus diagnostics.
    seq = property(lambda self: self.sequence)
    bid = property(lambda self: self.metadata["block_id"])
    dts = property(lambda self: self.metadata["device_timestamp_raw"])
    hrt = property(lambda self: self.metadata["host_realtime_ns"])
    pf = property(lambda self: (self.metadata["image"] or {}).get("pixel_format", 0))
    w = property(lambda self: (self.metadata["image"] or {}).get("width", 0))
    h = property(lambda self: (self.metadata["image"] or {}).get("height", 0))


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
            or metadata_size == 0):
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
    meta = json.loads(metadata_raw)
    _validate_metadata(meta, payload_size)
    return Record(Path(path), offset, seq, meta, metadata_size, payload_size, pcrc)


def iter_records(path: Path, *, live: bool = False) -> Iterator[Record]:
    """Scan a fixed-size snapshot, validating metadata while seeking over payloads."""
    with Path(path).open("rb") as source:
        size = os.fstat(source.fileno()).st_size
        segment_index(source)
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


# Payloads whose CRC-32C has already matched, keyed by location, size, CRC and file mtime.
_verified: set[tuple] = set()


def read_payload(record: Record, *, verify_crc: bool = False) -> bytes:
    with record.path.open("rb") as source:
        mtime = os.fstat(source.fileno()).st_mtime_ns
        source.seek(record.payload_offset)
        payload = source.read(record.payload_size)
    if len(payload) != record.payload_size:
        raise ValueError("eBUS payload became incomplete while reading")
    key = (str(record.path), record.offset, record.payload_size, record.payload_crc32c, mtime)
    if verify_crc and key not in _verified:
        if crc32c(payload) != record.payload_crc32c:
            raise ValueError(f"eBUS payload CRC-32C mismatch for record {record.sequence}")
        _verified.add(key)
    return payload


def session_end(directory: Path) -> dict:
    recording_format = device_metadata(Path(directory))["recording_format"]
    result = None
    with (Path(directory) / "events.jsonl").open("rb") as source:
        for raw in source:
            if not raw.endswith(b"\n"):
                raise ValueError("Incomplete eBUS event")
            event = json.loads(raw)
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
            if segment_index(source) != index:
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
    """The first valid image of a finalized session, its payload, and whether invalid records preceded it."""
    records = list(session_records(directory))  # read to the end so the session counters are verified
    first = next((index for index, record in enumerate(records) if record.valid_frame), None)
    if first is None:
        raise ValueError("No valid recorded image")
    return records[first], read_payload(records[first], verify_crc=verify_crc), first > 0


def _has_records(path: Path) -> bool:
    """False for a segment the writer has just created (header not yet complete) or that is gone."""
    try:
        return path.stat().st_size > FILE_HEADER_SIZE
    except FileNotFoundError:
        return False


def newest_valid_record(directory: Path, *, segments: int = 3) -> Record | None:
    """Newest committed valid image in a stream that rolls over to a new segment per record.

    Only the newest ``segments`` files are scanned, newest first. A segment that
    the writer has just created (header not yet complete, or no complete record
    yet) is skipped instead of failing, and a torn trailing record is ignored.
    """
    for path in reversed(segment_paths(directory)[-max(1, segments):]):
        if not _has_records(path):
            continue
        newest = None
        for record in iter_records(path, live=True):
            if record.valid_frame:
                newest = record
        if newest is not None:
            return newest
    return None


def latest_records(directory: Path, limit: int) -> list[Record]:
    """The newest ``limit`` valid records of the newest segment and its predecessor.

    A segment the writer has just created (header not yet complete) does not count.
    """
    selected = deque(maxlen=max(1, limit))
    for path in [path for path in segment_paths(directory)[-3:] if _has_records(path)][-2:]:
        selected.extend(record for record in iter_records(path, live=True) if record.valid_frame)
    return list(selected)
