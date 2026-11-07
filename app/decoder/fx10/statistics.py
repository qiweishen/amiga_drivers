"""FX10 spectral statistics in native digital numbers; saturation remains a percentage."""

from __future__ import annotations

import math
import numpy as np
from pathlib import Path

from .recording import load_capture, raw_frame_batches


def spectrum(cube: np.ndarray, full_scale: int) -> tuple[dict[str, list[float]], float, float]:
    """Per-band and overall DN statistics, plus the percentage at/above full scale.

    Values above nominal full scale are retained. No percentage scaling,
    display rounding, dark subtraction or white-reference correction is applied.
    """
    axes = (0, 2)
    values = {"mean": cube.mean(axis=axes), "median": np.median(cube, axis=axes),
              "max": cube.max(axis=axes), "min": cube.min(axis=axes), "p90": np.percentile(cube, 90, axis=axes)}
    return ({key: [float(v) for v in row] for key, row in values.items()},
            float(cube.mean()), float((cube >= full_scale).mean() * 100.0))


MAX_HISTOGRAM_BYTES = 128 * 1024 * 1024


class BandHistograms:
    def __init__(self, bands: int, full_scale: int):
        self.full_scale = full_scale
        self.memory_bytes = bands * (full_scale + 1) * 8
        if self.memory_bytes > MAX_HISTOGRAM_BYTES:
            raise ValueError("Reference geometry exceeds the statistics memory budget")
        self.histograms = [np.zeros(full_scale + 1, dtype=np.uint64) for _ in range(bands)]
        self.count = 0  # pixels per band, across accepted frames and spatial samples

    def add(self, cube: np.ndarray) -> None:
        added = cube.shape[0] * cube.shape[2]
        if self.count + added > 2 ** 53:
            raise ValueError("Reference is too large for exact percentile rank calculation")
        for band, histogram in enumerate(self.histograms):
            counts = np.bincount(cube[:, band, :].ravel(), minlength=len(histogram)).astype(np.uint64)
            if len(counts) > len(histogram):
                # Retain samples above nominal full scale rather than clipping.
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
            raise ValueError("No valid reference image frames")
        values = {name: [] for name in ("mean", "median", "max", "min", "p90")}
        total_dn = 0.0
        clipped = 0
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
                values[name].append(float(value))
            total_dn += dn_sum
            clipped += int(histogram[self.full_scale:].sum())
        total_pixels = self.count * len(self.histograms)
        return values, total_dn / total_pixels, clipped / total_pixels * 100.0


def reference_spectrum(directory: Path, expected_frames: int):
    """Stream one finalized reference phase into bounded raw-DN histograms."""
    capture = load_capture(directory)
    histograms = BandHistograms(capture.bands, capture.full_scale)
    count = 0
    for cube in raw_frame_batches(directory, capture):
        histograms.add(cube)
        count += cube.shape[0]
    if count != expected_frames:
        raise ValueError("Reference raw records disagree with the completed phase frame count")
    values, mean, clipped = histograms.finish()
    return capture, count, values, mean, clipped
