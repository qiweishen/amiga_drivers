"""FX10 timing limits shared by the Camera workbench and config writeback.

Mirrors fx10_driver/include/app_config.h (kMaxFrameRateHzUnbinned/Binned,
kMaxExposureMs, ExposureFitsPeriod); the C++ loader stays the authority at
acquisition start.
"""

from __future__ import annotations

import math

# FX10e AcquisitionFrameRate ceilings set by the operator (2026-11-08).
MAX_FRAME_RATE_HZ_UNBINNED = 163.02  # BinningHorizontal = BinningVertical = 1
MAX_FRAME_RATE_HZ_BINNED = 324.373  # every other binning combination
MIN_FRAME_RATE_HZ = 0.001  # the field's lower bound; the C++ loader only requires > 0
MAX_EXPOSURE_MS = 419.0  # ExposureTime maximum 419000 us
MIN_EXPOSURE_MS = 0.010  # ExposureTime minimum 10 us
PERIOD_FRACTION = 0.9  # the exposure may use at most 90 % of the frame period
RATE_STEP_HZ = 0.001


def max_rate_for_binning(spatial_binning: int, spectral_binning: int) -> float:
    return (MAX_FRAME_RATE_HZ_UNBINNED if int(spatial_binning) == 1 and int(spectral_binning) == 1
            else MAX_FRAME_RATE_HZ_BINNED)


def exposure_limit_ms(frame_rate_hz: float) -> float:
    """Longest exposure that fits ``frame_rate_hz``: 0.9 * 1000 / rate, ms."""
    return PERIOD_FRACTION * 1000.0 / frame_rate_hz


def exposure_fits(exposure_ms: float, frame_rate_hz: float) -> bool:
    """Inclusive, with the same 1e-9 relative allowance as the C++ check."""
    return exposure_ms <= exposure_limit_ms(frame_rate_hz) * (1.0 + 1e-9)


def max_frame_rate_hz(exposure_ms: float, spatial_binning: int, spectral_binning: int) -> float:
    """Highest frame rate, floored to 0.001 Hz, at which ``exposure_ms`` uses <= 90 % of the
    period and the binning ceiling holds."""
    exposure_ms = float(exposure_ms)
    if not math.isfinite(exposure_ms) or exposure_ms <= 0:
        raise ValueError("exposure_ms must be finite and positive")
    rate = min(max_rate_for_binning(spatial_binning, spectral_binning), PERIOD_FRACTION * 1000.0 / exposure_ms)
    rate = math.floor(rate / RATE_STEP_HZ + 1e-9) * RATE_STEP_HZ
    # Flooring only lowers the rate, so the exposure still fits; guard float edges anyway.
    while rate > MIN_FRAME_RATE_HZ and not exposure_fits(exposure_ms, rate):
        rate -= RATE_STEP_HZ
    return round(max(rate, MIN_FRAME_RATE_HZ), 3)


def check_exposure(exposure_ms: float) -> float:
    value = float(exposure_ms)
    if not math.isfinite(value) or not MIN_EXPOSURE_MS <= value <= MAX_EXPOSURE_MS:
        raise ValueError(f"exposure_ms must be within [{MIN_EXPOSURE_MS:g}, {MAX_EXPOSURE_MS:g}] ms")
    return value


def check_frame_rate(frame_rate_hz: float, spatial_binning: int, spectral_binning: int) -> float:
    rate = float(frame_rate_hz)
    ceiling = max_rate_for_binning(spatial_binning, spectral_binning)
    if not math.isfinite(rate) or not MIN_FRAME_RATE_HZ <= rate <= ceiling:
        raise ValueError(f"frame_rate_hz must be within [{MIN_FRAME_RATE_HZ:g}, {ceiling:g}] Hz "
                         f"at {int(spatial_binning)} x {int(spectral_binning)} binning")
    return rate
