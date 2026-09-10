"""Read-only, whole-phase FX10 spectral statistics across finalized ENVI segments.

Uses the snapshot/Data Live reductions over (frames, spatial samples), normalized
by capture.json full_scale. Integer histograms retain exact order statistics
(NumPy's default linear percentile interpolation) without loading a whole run.
"""

from __future__ import annotations

import csv
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .fx10_tools import _hdr_int, _hdr_wavelengths

_BATCH_BYTES = 8 * 1024 * 1024
_MAX_LINE_BYTES = 64 * 1024 * 1024
_MAX_HISTOGRAM_BYTES = 128 * 1024 * 1024


@dataclass(frozen=True)
class ReferenceSpectrum:
    ok: bool
    reason: str = ""
    wavelengths_nm: list[float] = field(default_factory=list)
    spectrum_pct: dict[str, list[float]] = field(default_factory=dict)
    frames: int = 0
    bands: int = 0
    samples: int = 0
    mean_pct: float = 0.0
    clipped_pct: float = 0.0


class _BandHistograms:
    def __init__(self, bands: int, full_scale: int):
        self.full_scale = full_scale
        self.memory_bytes = bands * (full_scale + 1) * 8
        if self.memory_bytes > _MAX_HISTOGRAM_BYTES:
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
                if self.memory_bytes + extra > _MAX_HISTOGRAM_BYTES:
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


def read_spectrum(session_dir: Path, expected_frames: int) -> ReferenceSpectrum:
    """Decode all finalized segments; a preview failure never changes raw data."""
    try:
        return _read_spectrum(session_dir, expected_frames)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError, csv.Error) as error:
        return ReferenceSpectrum(False, reason=f"Cannot display reference spectrum: {error}")


def _read_spectrum(session_dir: Path, expected_frames: int) -> ReferenceSpectrum:
    capture_path = session_dir / "capture.json"
    if capture_path.stat().st_size > 2 * 1024 * 1024:
        raise ValueError("Capture metadata is unexpectedly large")
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    if (not isinstance(capture, dict) or capture.get("format") != "fx10-capture-v1"
            or capture.get("byte_order") != "little"):
        raise ValueError("Unsupported capture metadata")
    samples, bands = int(capture["samples"]), int(capture["bands"])
    bpp, full_scale = int(capture["bytes_per_pixel"]), int(capture["full_scale"])
    if not (0 < samples <= 65535 and 0 < bands <= 65535):
        raise ValueError("Invalid capture geometry")
    if (bpp, full_scale) not in ((1, 255), (2, 1023), (2, 4095)):
        raise ValueError("Unknown capture bit depth")
    line_bytes = samples * bands * bpp
    if line_bytes > _MAX_LINE_BYTES:
        raise ValueError("Recorded line exceeds the preview memory budget")
    wavelengths = [float(v) for v in capture.get("wavelengths_nm", [])]
    if wavelengths and (len(wavelengths) != bands or not all(math.isfinite(v) and v > 0 for v in wavelengths)):
        raise ValueError("Invalid recorded wavelength axis")
    headers = sorted(session_dir.glob("segment_*.hdr"))
    if not headers:
        raise ValueError("No finalized ENVI segments")
    dtype = np.dtype("u1") if bpp == 1 else np.dtype("<u2")
    stats = _BandHistograms(bands, full_scale)
    frame_count = 0
    batch: list[bytes] = []

    def flush() -> None:
        if batch:
            cube = np.frombuffer(b"".join(batch), dtype=dtype).reshape(len(batch), bands, samples)
            stats.add(cube)
            batch.clear()

    for header in headers:
        if header.stat().st_size > 2 * 1024 * 1024:
            raise ValueError(f"Oversized ENVI header: {header.name}")
        text = header.read_text(encoding="utf-8")
        lines = _hdr_int(text, "lines")
        if (lines is None or lines <= 0 or _hdr_int(text, "samples") != samples
                or _hdr_int(text, "bands") != bands or _hdr_int(text, "byte order") != 0
                or _hdr_int(text, "header offset") != 0
                or _hdr_int(text, "data type") != (1 if bpp == 1 else 12)
                or not re.search(r"^interleave\s*=\s*bil\s*$", text, re.MULTILINE | re.IGNORECASE)
                or _hdr_wavelengths(text, bands) != wavelengths):
            raise ValueError(f"ENVI header disagrees with capture metadata: {header.name}")
        bil = header.with_suffix(".bil")
        if bil.stat().st_size != lines * line_bytes:
            raise ValueError(f"BIL length disagrees with finalized header: {bil.name}")
        previous_line = -1
        with header.with_suffix(".lines.csv").open(encoding="ascii", newline="") as index, bil.open("rb") as data:
            if not index.readline().startswith(("# fx10-line-index-v1;", "# fx10-line-index-v2;")):
                raise ValueError("Missing versioned line identity index")
            for row in csv.DictReader(index):
                if row["event"] != "frame" or row["block_id_anomaly"] != "0":
                    continue
                line, offset = int(row["segment_line"]), int(row["byte_offset"])
                if not previous_line < line < lines or offset != line * line_bytes:
                    raise ValueError("Invalid, duplicate or reversed reference line index")
                previous_line = line
                if batch and (len(batch) + 1) * line_bytes > _BATCH_BYTES:
                    flush()
                data.seek(offset)
                raw = data.read(line_bytes)
                if len(raw) != line_bytes:
                    raise ValueError("Reference data became incomplete while reading")
                batch.append(raw)
                frame_count += 1
    flush()
    if frame_count != expected_frames:
        raise ValueError("Reference frame index disagrees with the completed phase frame count")
    spectrum, mean, clipped = stats.finish()
    return ReferenceSpectrum(True, wavelengths_nm=wavelengths, spectrum_pct=spectrum,
                             frames=frame_count, bands=bands, samples=samples,
                             mean_pct=mean, clipped_pct=clipped)
