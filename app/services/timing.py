"""GOX and FX10 frame-rate / exposure limits shared by the Camera workbench and config writeback.

Mirrors gox_driver/include/app_config.h (kMaxFrameRateHz, kMaxFocusFrameRateHz,
kMaxExposureMs) and fx10_driver/include/app_config.h (kMaxFrameRateHzUnbinned /
Binned, kMaxExposureMs, ExposureFitsPeriod); the C++ loaders stay the authority
at acquisition start. Change both sides together.
"""

from __future__ import annotations

import math

PERIOD_FRACTION = 0.9  # the exposure may use at most 90 % of the frame period
RATE_STEP_HZ = 0.001

# GOX-12405C-PGE, full frame 4128 x 3008, BayerRG12Packed.
GOX_MAX_FRAME_RATE_HZ = 6.054  # AcquisitionFrameRate ceiling set by the operator (2026-11-08)
GOX_MAX_FOCUS_FRAME_RATE_HZ = 9.078  # focus-assist BayerRG8 stream only; displayed max 9.079 is not writable
GOX_MIN_FRAME_RATE_HZ = 0.125  # longest frame period 8 s (manual p.36/p.141)
GOX_MAX_EXPOSURE_MS = 399.405  # ExposureTime maximum 399405 us read back from the camera
GOX_MIN_EXPOSURE_MS = 0.001

# FX10e AcquisitionFrameRate ceilings set by the operator (2026-11-08).
FX10_MAX_FRAME_RATE_HZ_UNBINNED = 163.02  # BinningHorizontal = BinningVertical = 1
FX10_MAX_FRAME_RATE_HZ_BINNED = 324.373  # every other binning combination
FX10_MIN_FRAME_RATE_HZ = 0.001  # the field's lower bound; the C++ loader only requires > 0
FX10_MAX_EXPOSURE_MS = 419.0  # ExposureTime maximum 419000 us
FX10_MIN_EXPOSURE_MS = 0.010  # ExposureTime minimum 10 us


def positive(value, name: str) -> float:
    """A finite positive number (booleans rejected) as float."""
    try:
        number = float(value) if not isinstance(value, bool) else math.nan
    except (TypeError, ValueError, OverflowError):
        number = math.nan
    if not math.isfinite(number) or number <= 0:
        raise ValueError(f"{name} must be a finite positive number")
    return number


def exposure_limit_ms(frame_rate_hz: float) -> float:
    """Longest exposure that fits ``frame_rate_hz``: 0.9 * 1000 / rate, ms."""
    return PERIOD_FRACTION * 1000.0 / frame_rate_hz


def exposure_fits(exposure_ms: float, frame_rate_hz: float) -> bool:
    """Inclusive, with the same 1e-9 relative allowance as the C++ check."""
    return exposure_ms <= exposure_limit_ms(frame_rate_hz) * (1.0 + 1e-9)


def period_error(exposure_ms: float, frame_rate_hz: float) -> str:
    return (f"{exposure_ms:g} ms exceeds {PERIOD_FRACTION:.0%} of the frame period at {frame_rate_hz:g} Hz "
            f"(at most {exposure_limit_ms(frame_rate_hz):g} ms)")


def max_rate_hz(exposure_ms: float, ceiling_hz: float, floor_hz: float) -> float:
    """Highest rate, floored to 0.001 Hz and within [floor, ceiling], at which the exposure fits."""
    exposure_ms = positive(exposure_ms, "exposure_ms")
    rate = min(ceiling_hz, PERIOD_FRACTION * 1000.0 / exposure_ms)
    rate = math.floor(rate / RATE_STEP_HZ + 1e-9) * RATE_STEP_HZ
    # Flooring only lowers the rate, so the exposure still fits; guard float edges anyway.
    while rate > floor_hz and not exposure_fits(exposure_ms, rate):
        rate -= RATE_STEP_HZ
    return round(max(rate, floor_hz), 3)


def _in_range(value, low: float, high: float, name: str, unit: str, context: str = "") -> float:
    number = float(value)
    if not math.isfinite(number) or not low <= number <= high:
        raise ValueError(f"{name} must be within [{low:g}, {high:g}] {unit}{context}")
    return number


# --- GOX -----------------------------------------------------------------------

def gox_max_frame_rate_hz(exposure_ms: float, ceiling_hz: float = GOX_MAX_FRAME_RATE_HZ) -> float:
    """The focus-assist stream passes GOX_MAX_FOCUS_FRAME_RATE_HZ as the ceiling."""
    return max_rate_hz(exposure_ms, ceiling_hz, GOX_MIN_FRAME_RATE_HZ)


def gox_check_frame_rate(frame_rate_hz: float) -> float:
    return _in_range(frame_rate_hz, GOX_MIN_FRAME_RATE_HZ, GOX_MAX_FRAME_RATE_HZ, "frame_rate_hz", "Hz")


def gox_check_exposure(exposure_ms: float) -> float:
    return _in_range(exposure_ms, GOX_MIN_EXPOSURE_MS, GOX_MAX_EXPOSURE_MS, "Exposure", "ms")


# --- FX10 ----------------------------------------------------------------------

def fx10_rate_ceiling(spatial_binning: int, spectral_binning: int) -> float:
    return (FX10_MAX_FRAME_RATE_HZ_UNBINNED if int(spatial_binning) == 1 and int(spectral_binning) == 1
            else FX10_MAX_FRAME_RATE_HZ_BINNED)


def fx10_max_frame_rate_hz(exposure_ms: float, spatial_binning: int, spectral_binning: int) -> float:
    return max_rate_hz(exposure_ms, fx10_rate_ceiling(spatial_binning, spectral_binning), FX10_MIN_FRAME_RATE_HZ)


def fx10_check_exposure(exposure_ms: float) -> float:
    return _in_range(exposure_ms, FX10_MIN_EXPOSURE_MS, FX10_MAX_EXPOSURE_MS, "exposure_ms", "ms")


def fx10_check_frame_rate(frame_rate_hz: float, spatial_binning: int, spectral_binning: int) -> float:
    return _in_range(frame_rate_hz, FX10_MIN_FRAME_RATE_HZ, fx10_rate_ceiling(spatial_binning, spectral_binning),
                     "frame_rate_hz", "Hz", f" at {int(spatial_binning)} x {int(spectral_binning)} binning")
