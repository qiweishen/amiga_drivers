"""Bounded reads of committed GoX index records, checked against raw headers.

The repair CLI may scan beyond the index using layout.iter_records. Previews
only use complete JSONL rows, never an unindexed payload or a torn last row.
"""

from __future__ import annotations

import json
import os
import struct
from pathlib import Path
from typing import Iterator

from . import layout

MAX_PAYLOAD_BYTES = 128 * 1024 * 1024
INDEX_TAIL_BYTES = 256 * 1024
_MAX_INDEX_ROW_BYTES = 8192
_IDENTITY = ("off", "psz", "seq", "bid", "dts", "hrt", "hmn", "pf", "w", "h", "fl")


def _uint(value, field):
    if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
        raise ValueError(f"Invalid GoX index {field}")
    return value


def index_records(path: Path, *, tail_bytes: int | None = None) -> Iterator[dict]:
    """A bounded suffix or streaming prefix of complete JSONL records."""
    with path.open("rb") as source:
        size = os.fstat(source.fileno()).st_size
        start = max(0, size - tail_bytes) if tail_bytes is not None else 0
        if start:
            source.seek(start - 1)
            if source.read(1) != b"\n":
                skipped = source.readline(_MAX_INDEX_ROW_BYTES + 1)
                if len(skipped) > _MAX_INDEX_ROW_BYTES:
                    raise ValueError("Oversized GoX index row")
        previous = None
        while source.tell() < size:
            raw = source.readline(min(_MAX_INDEX_ROW_BYTES + 1, size - source.tell()))
            if len(raw) > _MAX_INDEX_ROW_BYTES:
                raise ValueError("Oversized GoX index row")
            if not raw.endswith(b"\n"):
                break
            row = json.loads(raw.decode("ascii"))
            if not isinstance(row, dict):
                raise ValueError("GoX index row is not an object")
            for field in _IDENTITY:
                _uint(row.get(field), field)
            if previous is not None and (row["off"] <= previous["off"] or row["seq"] <= previous["seq"]):
                raise ValueError("Duplicate or reversed GoX index records")
            previous = row
            yield row


def segment_for_index(index: Path) -> Path:
    if not index.name.endswith(".idx.jsonl"):
        raise ValueError("Not a GoX segment index")
    return index.with_name(index.name.removesuffix(".idx.jsonl") + ".raw")


def read_indexed_frame(segment: Path, row: dict, *, verify_payload_crc: bool = False):
    """Read one complete tight image and reject index/header disagreement.

    Header CRC is always checked. Full payload CRC is an explicit caller policy
    (the offline verify CLI checks it); interactive previews avoid that extra
    whole-image Python pass. SDK-only layout must be available in the index.
    """
    for field in _IDENTITY:
        _uint(row.get(field), field)
    if row["fl"] & (layout.FRAME_FLAG_INCOMPLETE | layout.FRAME_FLAG_RESULT_NOT_OK):
        raise ValueError("Frame is INCOMPLETE or has an SDK receive error")
    for field in ("padding_x", "padding_y", "chunk_count"):
        if field not in row or row[field] is None:
            raise ValueError("Frame layout metadata is unavailable; refusing to guess row stride")
        if _uint(row[field], field) != 0:
            raise ValueError("Preview does not support padded/chunked payloads; raw data is retained")
    if row["fl"] & layout.FRAME_FLAG_CHUNK_DATA:
        raise ValueError("Preview does not support chunk-bearing frames")
    if not 0 < row["psz"] <= MAX_PAYLOAD_BYTES:
        raise ValueError("Frame size exceeds preview bounds")
    with segment.open("rb") as source:
        size = os.fstat(source.fileno()).st_size
        header, problems = layout.read_file_header(source, str(segment), size)
        if problems:
            raise ValueError("Invalid GoX file header: " + "; ".join(problems))
        if row["off"] < layout.align_up(layout.FILE_HEADER_SIZE, header.align) or row["off"] % header.align:
            raise ValueError("GoX record offset violates the segment alignment")
        end = row["off"] + layout.FRAME_HEADER_SIZE + row["psz"]
        if end > size:
            raise ValueError("Newest frame is not fully on disk yet — retry")
        source.seek(row["off"])
        raw = source.read(layout.FRAME_HEADER_SIZE)
        if len(raw) != layout.FRAME_HEADER_SIZE:
            raise ValueError("GoX frame header became incomplete while reading")
        problem = layout.frame_header_ok(raw)
        if problem:
            raise ValueError(problem)
        frame = layout.Frame(row["off"], *struct.unpack(layout.FRAME_HEADER_FMT, raw))
        for field in _IDENTITY + ("ox", "oy"):
            if field in ("ox", "oy") and field not in row:
                continue  # v1 raw headers retain offsets even when old indexes omitted them
            if _uint(row.get(field), field) != getattr(frame, field):
                raise ValueError(f"GoX index disagrees with frame header: {field}")
        payload = source.read(frame.psz)
        if len(payload) != frame.psz:
            raise ValueError("GoX payload became incomplete while reading")
        if verify_payload_crc and header.flags & layout.SEG_FLAG_PAYLOAD_CRC and layout.crc32c(payload) != frame.pcrc:
            raise ValueError("GoX payload CRC-32C mismatch")
    return frame, payload


def first_preview_frame(camera_dir: Path):
    """Snapshot access: first complete indexed frame, bounded scan, no salvage."""
    skipped_incomplete = False
    examined = 0
    for index in sorted(camera_dir.glob("seg_*.idx.jsonl")):
        for row in index_records(index):
            examined += 1
            if examined > 4096:
                raise ValueError("Snapshot index exceeds the preview scan budget")
            if row["fl"] & (layout.FRAME_FLAG_INCOMPLETE | layout.FRAME_FLAG_RESULT_NOT_OK):
                skipped_incomplete = True
                continue
            frame, payload = read_indexed_frame(segment_for_index(index), row)
            return frame, payload, skipped_incomplete
    raise ValueError("No complete committed GoX frame found")
