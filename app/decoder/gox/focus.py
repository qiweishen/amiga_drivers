"""Focus sharpness on GOX Bayer photosites (Camera Tools focus assist).

The focus-assist stream trades depth for frame rate: ``jai_snapshot --stream-s``
records 8-bit BayerRG8, at the camera's factory A/D depth, which reaches 9.079 Hz
at the full frame instead of 6.054 Hz for BayerRG12Packed. This module therefore
reads BayerRG8 only. Recordings, snapshots and references stay BayerRG12Packed.

Only one green CFA plane is scored: the Gr photosites of BayerRG (row 0,
column 1 of every 2 x 2 tile), read straight from the payload bytes without
demosaicing, so the score follows the lens rather than an interpolation filter:

    score = var(Laplacian(G)) / (mean(G) - pedestal)^2

The pedestal is the documented default black level, 8 LSB at 8 bit (manual
Ver. 1.3 p.172; the same level as the 128 DN used at 12 bit). A constant
pedestal does not change the Laplacian. Dividing by the squared mean signal
above it makes the score independent of illumination and exposure, so it tracks
focus. Without the subtraction, a passing cloud would lower the score at
unchanged focus. A score compares only within one setup (target, ROI, aperture,
light), so the GUI shows it relative to the session peak. A ROI whose mean
signal is below ``MIN_SIGNAL_DN`` is not scored.

Screen images are the 8-bit photosite values themselves (linear DN / 255): no
stretch, tone curve or requantization. The RAW payload is only read.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import cv2
import numpy as np

from ..ebus import pixels
from .color_checker import DOCUMENTED_PEDESTAL_DN

BAYER_RG8_CODE = 0x01080009  # PFNC BayerRG8: 1 px / 1 B
FOCUS_PIXEL_FORMAT = "BayerRG8"  # gox::kFocusPixelFormat
FULL_SCALE_DN = 255
PEDESTAL_DN = DOCUMENTED_PEDESTAL_DN / 16.0  # 8 LSB at 8 bit = 128 DN at 12 bit
MIN_SIGNAL_DN = 4.0  # mean green signal above the pedestal needed for a score (64 DN at 12 bit)
GREEN_ROW, GREEN_COLUMN = 0, 1  # the Gr photosite of the BayerRG 2 x 2 tile
MIN_HALF_PX = 32  # ROI half-size limits, full-resolution pixels
MAX_HALF_PX = 1024
LOUPE_MAX_PX = 256  # magnifier edge, green photosites (the ROI centre when larger)
OVERVIEW_LONG_EDGE = 960


@dataclass(frozen=True)
class Roi:
    """Full-resolution window [x0, x1) x [y0, y1), even-aligned to whole 2 x 2 tiles."""

    x0: int
    y0: int
    x1: int
    y1: int

    @property
    def center(self) -> tuple[int, int]:
        return (self.x0 + self.x1) // 2, (self.y0 + self.y1) // 2


@dataclass(frozen=True)
class FocusSample:
    seq: int
    width: int
    height: int
    roi: Roi
    score: float | None  # None: ROI too dark (or too small) to score
    signal_dn: float  # mean Gr DN above the documented pedestal
    clipped_pct: float  # ROI photosites of every CFA colour at full scale
    loupe_png: bytes  # 8-bit grayscale Gr photosites, one image pixel per photosite
    loupe_origin: tuple[int, int]  # full-resolution (x, y) of the magnifier's top-left tile
    overview_png: bytes | None = None
    overview_scale: tuple[float, float] = (1.0, 1.0)  # full-resolution px per overview px (x, y)


def roi_window(width: int, height: int, center: tuple[float, float] | None, half: int) -> Roi:
    """A 2*half square around ``center`` (the frame centre when None), moved inside the frame."""
    if width < 2 or height < 2:
        raise ValueError("Frame is too small for a focus window")
    half = min(max(int(half), MIN_HALF_PX), MAX_HALF_PX)
    cx, cy = (width / 2.0, height / 2.0) if center is None else (float(center[0]), float(center[1]))
    if not (math.isfinite(cx) and math.isfinite(cy)):
        raise ValueError("ROI centre must be finite")
    size_x = min(2 * half, width - width % 2)
    size_y = min(2 * half, height - height % 2)
    x0 = min(max(math.floor(cx - size_x / 2.0) & ~1, 0), (width - size_x) & ~1)
    y0 = min(max(math.floor(cy - size_y / 2.0) & ~1, 0), (height - size_y) & ~1)
    return Roi(x0, y0, x0 + size_x, y0 + size_y)


def sharpness(green: np.ndarray, pedestal_dn: float = PEDESTAL_DN) -> tuple[float | None, float]:
    """(score, mean signal above the pedestal in DN); the score is None when the ROI is too dark."""
    values = np.ascontiguousarray(green, dtype=np.float32)
    if values.ndim != 2 or values.size == 0:
        raise ValueError("Sharpness needs a nonempty 2-D photosite plane")
    signal = float(values.mean(dtype=np.float64)) - pedestal_dn
    if min(values.shape) < 3 or signal < MIN_SIGNAL_DN:
        return None, signal
    laplacian = cv2.Laplacian(values, cv2.CV_32F, ksize=3)
    return float(laplacian.var(dtype=np.float64)) / (signal * signal), signal


def gray_png(gray8: np.ndarray) -> bytes:
    gray8 = np.ascontiguousarray(gray8)
    if gray8.dtype != np.uint8 or gray8.ndim != 2 or gray8.size == 0:
        raise ValueError("Grayscale PNG encoding requires a nonempty 2-D uint8 image")
    ok, encoded = cv2.imencode(".png", gray8, [cv2.IMWRITE_PNG_COMPRESSION, 1])
    data = encoded.tobytes() if ok else b""
    if len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[24] != 8 or data[25] != 0:
        raise ValueError("PNG encoder did not produce an 8-bit grayscale image")
    return data


def _overview(image: np.ndarray) -> tuple[bytes, tuple[float, float]]:
    height, width = image.shape
    plane = image[GREEN_ROW::2, GREEN_COLUMN::2]
    plane_h, plane_w = plane.shape
    scale = min(1.0, OVERVIEW_LONG_EDGE / max(plane_h, plane_w))
    size = (max(1, round(plane_w * scale)), max(1, round(plane_h * scale)))
    plane = np.ascontiguousarray(plane)
    if scale < 1.0:
        plane = cv2.resize(plane, size, interpolation=cv2.INTER_AREA)
    return gray_png(plane), (width / size[0], height / size[1])


def analyze(record, payload: bytes, center: tuple[float, float] | None, half: int, *,
            overview: bool = False) -> FocusSample:
    """Score one committed BayerRG8 record's ROI; optionally also render the overview."""
    if record.pf != BAYER_RG8_CODE:
        raise ValueError(f"GOX focus requires {FOCUS_PIXEL_FORMAT}; received PFNC 0x{record.pf:08X}")
    # BayerRG8 rows are the photosites themselves: a (height, width) uint8 view, nothing decoded.
    image = pixels.image_rows(record, payload)
    height, width = image.shape
    roi = roi_window(width, height, center, half)
    # y0 and x0 are even, so the window starts on an R/Gr row and the tile phase is kept.
    bayer = image[roi.y0:roi.y1, roi.x0:roi.x1]
    green = bayer[GREEN_ROW::2, GREEN_COLUMN::2]
    score, signal = sharpness(green)
    clipped = float(np.count_nonzero(bayer >= FULL_SCALE_DN)) / bayer.size * 100.0
    green_h, green_w = green.shape
    loupe_w, loupe_h = min(green_w, LOUPE_MAX_PX), min(green_h, LOUPE_MAX_PX)
    off_x, off_y = (green_w - loupe_w) // 2, (green_h - loupe_h) // 2
    loupe = green[off_y:off_y + loupe_h, off_x:off_x + loupe_w]
    overview_png, overview_scale = _overview(image) if overview else (None, (1.0, 1.0))
    return FocusSample(
        seq=record.seq, width=width, height=height, roi=roi, score=score, signal_dn=signal,
        clipped_pct=clipped, loupe_png=gray_png(loupe),
        loupe_origin=(roi.x0 + 2 * off_x, roi.y0 + 2 * off_y),
        overview_png=overview_png, overview_scale=overview_scale,
    )
