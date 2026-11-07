"""Display encoding of scene-linear GOX sRGB; never used for fitting or recording.

Input is the unclipped float32 linear sRGB (D65) produced by the color matrix.
The display image applies no exposure adjustment, tone curve or per-image
statistic: linear values are clipped to [0, 1] and encoded with the sRGB OETF,
so every frame is shown on the same fixed scale. Inputs are never modified.
"""

from __future__ import annotations

import numpy as np

from .preview import DISPLAY_LONG_EDGE, downscale, png8_bytes, png16_bytes

PROVENANCE = {
    "scope": "display only; scene-linear values, fitting and RAW are unchanged",
    "exposure_adjustment": "none",
    "encoding": "linear sRGB clipped to [0, 1] (negative and over-range values clipped), sRGB OETF",
    "screen_image": f"area-downscaled to <= {DISPLAY_LONG_EDGE} px, 8-bit PNG",
    "download_image": "full resolution, 16-bit PNG",
}


def srgb_oetf(linear: np.ndarray) -> np.ndarray:
    value = np.clip(linear, 0.0, 1.0)
    return np.where(value <= 0.0031308, value * 12.92, 1.055 * np.power(value, 1.0 / 2.4) - 0.055)


def _check_linear(linear: np.ndarray) -> None:
    if (not isinstance(linear, np.ndarray) or linear.dtype != np.float32 or linear.ndim != 3
            or linear.shape[2] != 3 or linear.size == 0 or not np.isfinite(linear).all()):
        raise ValueError("Display encoding requires a nonempty finite float32 (H, W, 3) linear image")


def display_png(linear: np.ndarray, long_edge: int = DISPLAY_LONG_EDGE) -> bytes:
    """8-bit on-screen PNG, area-downscaled in linear light before encoding."""
    _check_linear(linear)
    encoded = srgb_oetf(downscale(linear, long_edge))
    return png8_bytes(np.rint(encoded * 255.0).astype(np.uint8))


def full_png16(linear: np.ndarray) -> bytes:
    """Full-resolution 16-bit PNG with the same fixed encoding as the screen image."""
    _check_linear(linear)
    return png16_bytes(np.rint(srgb_oetf(linear) * 65535.0).astype(np.uint16))
