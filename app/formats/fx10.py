"""FX10 capture-v1 / ENVI BIL / line-index-v1,v2 recording contract.

Full snapshots, bounded live tails and streaming reference reductions share this
parser. A newline-terminated frame/padding index row commits one BIL line;
padding, rejected buffers and anomalous frames never enter spectral statistics.
Live tails validate their sampled suffix, not the unexamined earlier recording.
No import starts acquisition, writes files, or imports application services.
"""

from __future__ import annotations

import csv
import json
import math
import os
import re
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import numpy as np

METADATA_BYTES = 2 * 1024 * 1024
MAX_LINE_BYTES = 64 * 1024 * 1024
INDEX_TAIL_BYTES = 256 * 1024
_INDEX_ROW_BYTES = 8192
_UINT64_MAX = (1 << 64) - 1
_PIXELS = {"Mono8": (1, 255), "Mono10": (2, 1023), "Mono10Packed": (2, 1023),
           "Mono12": (2, 4095), "Mono12Packed": (2, 4095)}
_COLUMNS_V1 = ("event", "segment_line", "global_line", "byte_offset", "block_id", "count",
               "device_timestamp_raw", "host_receive_realtime_ns", "host_receive_monotonic_ns",
               "block_id_anomaly")
_COLUMNS_V2 = _COLUMNS_V1 + ("sdk_acquired_size", "sdk_payload_type", "sdk_operation_result",
    "sdk_chunk_count", "sdk_image_present", "sdk_pixel_type", "sdk_width", "sdk_height",
    "sdk_padding_x", "sdk_padding_y", "sdk_image_size", "sdk_effective_image_size")


def _uint(value: object, name: str, *, positive: bool = False) -> int:
    if type(value) is not int or not (int(positive) <= value <= _UINT64_MAX):
        raise ValueError(f"Invalid {name}")
    return value


def _decimal(value: str, name: str) -> int:
    if not value or not value.isascii() or not value.isdecimal():
        raise ValueError(f"Invalid line-index {name}")
    return _uint(int(value), name)


def _text(path: Path) -> str:
    with path.open("rb") as source:
        raw = source.read(METADATA_BYTES + 1)
    if len(raw) > METADATA_BYTES:
        raise ValueError(f"Metadata exceeds size limit: {path.name}")
    return raw.decode("utf-8")


def _axis(values: object, bands: int, name: str) -> tuple[float, ...]:
    if not isinstance(values, (list, tuple)):
        raise ValueError(f"Invalid {name} axis")
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) for v in values):
        raise ValueError(f"Invalid {name} axis")
    out = tuple(float(v) for v in values)
    if out and (len(out) != bands or not all(math.isfinite(v) and v > 0 for v in out)):
        raise ValueError(f"Invalid {name} axis")
    return out


@dataclass(frozen=True)
class Capture:
    samples: int
    bands: int
    bytes_per_pixel: int
    full_scale: int
    pixel_format: str
    wavelengths_nm: tuple[float, ...]
    expected_frame_rate_hz: float
    line_index_format: str | None

    @property
    def line_bytes(self) -> int:
        return self.samples * self.bands * self.bytes_per_pixel

    @property
    def dtype(self) -> np.dtype:
        return np.dtype("u1" if self.bytes_per_pixel == 1 else "<u2")


def load_capture(directory: Path) -> Capture:
    doc = json.loads(_text(directory / "capture.json"))
    if not isinstance(doc, dict) or doc.get("format") != "fx10-capture-v1" or doc.get("byte_order") != "little":
        raise ValueError("Unsupported capture metadata")
    samples = _uint(doc["samples"], "capture samples", positive=True)
    bands = _uint(doc["bands"], "capture bands", positive=True)
    bpp = _uint(doc["bytes_per_pixel"], "capture bytes_per_pixel", positive=True)
    full_scale = _uint(doc["full_scale"], "capture full_scale", positive=True)
    pixel_format = doc["pixel_format"]
    if not isinstance(pixel_format, str) or _PIXELS.get(pixel_format) != (bpp, full_scale):
        raise ValueError("Capture PixelFormat, storage width and full scale disagree")
    if samples > 65535 or bands > 65535 or samples * bands * bpp > MAX_LINE_BYTES:
        raise ValueError("Capture geometry exceeds the preview memory budget")
    rate = doc["expected_frame_rate_hz"]
    if isinstance(rate, bool) or not isinstance(rate, (int, float)) or not math.isfinite(rate) or not 0 <= rate <= 100000:
        raise ValueError("Invalid capture frame rate")
    version = doc.get("line_index_format")
    if version not in (None, "fx10-line-index-v1", "fx10-line-index-v2"):
        raise ValueError("Unsupported capture line-index version")
    return Capture(samples, bands, bpp, full_scale, pixel_format,
                   _axis(doc.get("wavelengths_nm", []), bands, "wavelength"), float(rate), version)


def _header_fields(text: str) -> dict[str, str]:
    if not text.splitlines() or text.splitlines()[0].strip().upper() != "ENVI":
        raise ValueError("Missing ENVI header signature")
    fields = {}
    for match in re.finditer(r"^\s*([^=\r\n]+?)\s*=\s*(\{[^}]*\}|[^\r\n]*)", text, re.MULTILINE):
        key = " ".join(match.group(1).lower().split())
        if key in fields:
            raise ValueError(f"Duplicate ENVI field: {key}")
        fields[key] = match.group(2).strip()
    return fields


def _header_axis(fields: dict[str, str], name: str, bands: int) -> tuple[float, ...]:
    if name not in fields:
        return ()
    text = fields[name]
    if not text.startswith("{") or not text.endswith("}"):
        raise ValueError(f"Invalid ENVI {name}")
    values = [float(v.strip()) for v in text[1:-1].split(",") if v.strip()]
    if not values:
        raise ValueError(f"Empty ENVI {name}")
    return _axis(values, bands, name)


@dataclass(frozen=True)
class Segment:
    data: Path
    index: Path
    capture: Capture
    lines: int | None  # None only for an unfinished .bil.part


def open_segment(data: Path, capture: Capture) -> Segment:
    """Validate immutable layout; a finalized .bil must have its matching .hdr."""
    live = data.name.endswith(".bil.part")
    base = data.name.removesuffix(".part").removesuffix(".bil")
    index = data.with_name(base + (".lines.csv.part" if live else ".lines.csv"))
    if live:
        return Segment(data, index, capture, None)
    fields = _header_fields(_text(data.with_name(base + ".hdr")))
    expected = {"samples": capture.samples, "bands": capture.bands, "header offset": 0,
                "byte order": 0, "data type": 1 if capture.bytes_per_pixel == 1 else 12}
    for key, value in expected.items():
        if _decimal(fields.get(key, ""), "ENVI " + key) != value:
            raise ValueError(f"ENVI {key} disagrees with capture metadata")
    lines = _decimal(fields.get("lines", ""), "ENVI lines")
    if fields.get("interleave", "").lower() != "bil":
        raise ValueError("Only ENVI BIL interleave is supported")
    wavelengths = _header_axis(fields, "wavelength", capture.bands)
    if wavelengths != capture.wavelengths_nm:
        raise ValueError("ENVI wavelengths disagree with capture metadata")
    if wavelengths and fields.get("wavelength units", "").lower() != "nanometers":
        raise ValueError("ENVI wavelength units must be Nanometers")
    _header_axis(fields, "fwhm", capture.bands)
    if data.stat().st_size != lines * capture.line_bytes:
        raise ValueError("BIL length disagrees with finalized ENVI header")
    return Segment(data, index, capture, lines)


def finalized_segments(directory: Path, capture: Capture) -> list[Segment]:
    headers = sorted(directory.glob("segment_*.hdr"))
    if not headers:
        raise ValueError("No finalized ENVI segments")
    return [open_segment(path.with_suffix(".bil"), capture) for path in headers]


@dataclass(frozen=True)
class Line:
    event: str
    number: int
    offset: int
    global_number: int
    anomaly: bool

    @property
    def is_frame(self) -> bool:
        return self.event == "frame" and not self.anomaly


def _index_header(source, capture: Capture) -> tuple[int, int]:
    marker = source.readline(_INDEX_ROW_BYTES + 1)
    match = re.fullmatch(rb"# (fx10-line-index-v[12]);[^\r\n]*\r?\n", marker)
    if match is None:
        raise ValueError("Missing versioned line identity index")
    version = match.group(1).decode("ascii")
    if capture.line_index_format is not None and version != capture.line_index_format:
        raise ValueError("Line-index version disagrees with capture metadata")
    names = source.readline(_INDEX_ROW_BYTES + 1)
    if not names.endswith(b"\n"):
        raise ValueError("Incomplete line-index column header")
    columns = tuple(next(csv.reader([names.decode("ascii").rstrip("\r\n")], strict=True)))
    expected = _COLUMNS_V1 if version.endswith("v1") else _COLUMNS_V2
    if columns != expected:
        raise ValueError("Unexpected line-index columns")
    return len(expected), source.tell()


def _line(raw: bytes, columns: int, capture: Capture) -> Line:
    row = next(csv.reader([raw.decode("ascii").rstrip("\r\n")], strict=True))
    if len(row) != columns:
        raise ValueError("Invalid line-index column count")
    if not row[0] or not re.fullmatch(r"[a-z][a-z-]*", row[0]):
        raise ValueError("Invalid line-index event")
    numbers = [_decimal(value, _COLUMNS_V1[i]) for i, value in enumerate(row[1:10], 1)]
    number, global_number, offset, _, count, _, _, _, anomaly = numbers
    if offset != number * capture.line_bytes or count == 0 or anomaly not in (0, 1):
        raise ValueError("Invalid line-index offset, count or anomaly flag")
    if row[0] in ("frame", "padding") and count != 1:
        raise ValueError("A stored line must represent exactly one frame or padding line")
    for i, value in enumerate(row[10:], 10):
        if value:
            parsed = _decimal(value, _COLUMNS_V2[i])
            if _COLUMNS_V2[i] == "sdk_image_present" and parsed not in (0, 1):
                raise ValueError("Invalid SDK image-present flag")
    return Line(row[0], number, offset, global_number, bool(anomaly))


def committed_lines(segment: Segment, data_bytes: int, *, tail_bytes: int | None = None) -> Iterator[Line]:
    """Read a newline-committed prefix or a bounded suffix of that prefix.

    A data-size snapshot may precede concurrently appended index rows. Those
    newer rows are outside this read's prefix; a finalized length mismatch is
    always an error. Only a live trailing partial row may be discarded.
    """
    with segment.index.open("rb") as source:
        columns, header_end = _index_header(source, segment.capture)
        size = os.fstat(source.fileno()).st_size
        start = max(header_end, size - tail_bytes) if tail_bytes is not None else header_end
        if start > header_end:
            source.seek(start - 1)
            if source.read(1) != b"\n":
                source.readline(_INDEX_ROW_BYTES + 1)
                if source.tell() < size and source.tell() - start > _INDEX_ROW_BYTES:
                    raise ValueError("Oversized line-index row")
        else:
            source.seek(header_end)
        expected_line = None if start > header_end else 0
        global_base = None
        while source.tell() < size:
            raw = source.readline(min(_INDEX_ROW_BYTES + 1, size - source.tell()))
            if len(raw) > _INDEX_ROW_BYTES:
                raise ValueError("Oversized line-index row")
            if not raw.endswith(b"\n"):
                if segment.lines is not None:
                    raise ValueError("Finalized line-index has an incomplete trailing row")
                break
            line = _line(raw, columns, segment.capture)
            if expected_line is None:
                expected_line = line.number
            if global_base is None:
                global_base = line.global_number - line.number
            if line.number != expected_line or global_base < 0 or line.global_number != global_base + line.number:
                raise ValueError("Missing, duplicate or reversed line-index positions")
            stored = line.event in ("frame", "padding")
            end = line.offset + (segment.capture.line_bytes if stored else 0)
            if end > data_bytes:
                if segment.lines is not None:
                    raise ValueError("Line-index refers beyond finalized BIL data")
                break
            if stored:
                expected_line += 1
            yield line
        if segment.lines is not None and expected_line != segment.lines:
            raise ValueError("Finalized BIL lines are not fully represented by the committed index")


def frame_batches(segment: Segment, *, batch_bytes: int = 8 * 1024 * 1024) -> Iterator[np.ndarray]:
    """Stream every accepted frame without allocating a whole phase."""
    capture = segment.capture
    batch = []
    with segment.data.open("rb") as data:
        size = os.fstat(data.fileno()).st_size
        for line in committed_lines(segment, size):
            if not line.is_frame:
                continue
            data.seek(line.offset)
            raw = data.read(capture.line_bytes)
            if len(raw) != capture.line_bytes:
                raise ValueError("BIL data became incomplete while reading")
            batch.append(raw)
            if len(batch) * capture.line_bytes >= batch_bytes:
                yield np.frombuffer(b"".join(batch), dtype=capture.dtype).reshape(-1, capture.bands, capture.samples)
                batch.clear()
    if batch:
        yield np.frombuffer(b"".join(batch), dtype=capture.dtype).reshape(-1, capture.bands, capture.samples)


def snapshot_cube(directory: Path) -> tuple[Capture, np.ndarray]:
    capture = load_capture(directory)
    segments = finalized_segments(directory, capture)
    if len(segments) != 1:
        raise ValueError("Snapshot unexpectedly contains multiple segments")
    segment = segments[0]
    if segment.lines > 4096 or segment.lines * capture.line_bytes > 256 * 1024 * 1024:
        raise ValueError("Snapshot exceeds preview memory budget")
    chunks = list(frame_batches(segment))
    if not chunks:
        raise ValueError("No non-anomalous camera frames in the line index")
    return capture, np.concatenate(chunks, axis=0) if len(chunks) > 1 else chunks[0]


def tail_cube(segment: Segment, limit: int, *, tail_bytes: int = INDEX_TAIL_BYTES) -> np.ndarray:
    """Bounded live access; the earlier unsampled index is not audited here."""
    capture = segment.capture
    limit = min(max(1, limit), max(1, MAX_LINE_BYTES // capture.line_bytes))
    with segment.data.open("rb") as data:
        size = os.fstat(data.fileno()).st_size
        lines = deque((line for line in committed_lines(segment, size, tail_bytes=tail_bytes) if line.is_frame), maxlen=limit)
        frames = []
        for line in lines:
            data.seek(line.offset)
            raw = data.read(capture.line_bytes)
            if len(raw) != capture.line_bytes:
                raise ValueError("Committed BIL line became incomplete while reading")
            frames.append(raw)
    if not frames:
        raise ValueError("No complete non-anomalous line on disk yet — retry")
    return np.frombuffer(b"".join(frames), dtype=capture.dtype).reshape(-1, capture.bands, capture.samples)


def spectrum(cube: np.ndarray, full_scale: int) -> tuple[dict[str, list[float]], float, float]:
    axes = (0, 2)
    scale = 100.0 / full_scale
    values = {"mean": cube.mean(axis=axes), "median": np.median(cube, axis=axes),
              "max": cube.max(axis=axes), "min": cube.min(axis=axes), "p90": np.percentile(cube, 90, axis=axes)}
    return ({key: [round(float(v) * scale, 3) for v in row] for key, row in values.items()},
            float(cube.mean() * scale), float((cube >= full_scale).mean() * 100.0))


MAX_HISTOGRAM_BYTES = 128 * 1024 * 1024


class BandHistograms:
    def __init__(self, bands: int, full_scale: int):
        self.full_scale = full_scale
        self.memory_bytes = bands * (full_scale + 1) * 8
        if self.memory_bytes > MAX_HISTOGRAM_BYTES:
            raise ValueError("Reference geometry exceeds the statistics memory budget")
        self.histograms = [np.zeros(full_scale + 1, dtype=np.uint64) for _ in range(bands)]
        self.count = 0  # pixels PER band, across every accepted frame and spatial sample

    def add(self, cube: np.ndarray) -> None:
        added = cube.shape[0] * cube.shape[2]
        if self.count + added > 2 ** 53:
            raise ValueError("Reference is too large for exact percentile rank calculation")
        for band, histogram in enumerate(self.histograms):
            counts = np.bincount(cube[:, band, :].ravel(), minlength=len(histogram)).astype(np.uint64)
            if len(counts) > len(histogram):
                # Preserve container values above the nominal full scale too,
                # exactly as snapshot/Data Live do; never clip them before reduction.
                extra = (len(counts) - len(histogram)) * 8
                if self.memory_bytes + extra > MAX_HISTOGRAM_BYTES:
                    raise ValueError("Reference values exceed the statistics memory budget")
                histogram = np.pad(histogram, (0, len(counts) - len(histogram)))
                self.histograms[band] = histogram
                self.memory_bytes += extra
            histogram += counts
        self.count += added

    def finish(self) -> tuple[dict[str, list[float]], float, float]:
        if self.count == 0:
            raise ValueError("No non-anomalous reference frames in the line index")
        values = {name: [] for name in ("mean", "median", "max", "min", "p90")}
        total_dn = 0.0
        clipped = 0
        scale = 100.0 / self.full_scale
        for histogram in self.histograms:
            cumulative = np.cumsum(histogram, dtype=np.uint64)

            def percentile(q: float) -> float:
                rank = (self.count - 1) * q
                low, high = math.floor(rank), math.ceil(rank)
                a = int(np.searchsorted(cumulative, low, side="right"))
                b = int(np.searchsorted(cumulative, high, side="right"))
                return a + (b - a) * (rank - low)

            dn_sum = float(np.dot(histogram, np.arange(len(histogram), dtype=np.float64)))
            band_values = {"mean": dn_sum / self.count, "median": percentile(0.5),
                           "max": percentile(1.0), "min": percentile(0.0), "p90": percentile(0.9)}
            for name, value in band_values.items():
                values[name].append(round(value * scale, 3))
            total_dn += dn_sum
            clipped += int(histogram[self.full_scale:].sum())
        total_pixels = self.count * len(self.histograms)
        return values, total_dn / total_pixels * scale, clipped / total_pixels * 100.0

def reference_spectrum(directory: Path, expected_frames: int):
    """All finalized segments, exact integer histograms, bounded working memory."""
    capture = load_capture(directory)
    histograms = BandHistograms(capture.bands, capture.full_scale)
    count = 0
    for segment in finalized_segments(directory, capture):
        for cube in frame_batches(segment):
            histograms.add(cube)
            count += cube.shape[0]
    if count != expected_frames:
        raise ValueError("Reference frame index disagrees with the completed phase frame count")
    values, mean, clipped = histograms.finish()
    return capture, count, values, mean, clipped
