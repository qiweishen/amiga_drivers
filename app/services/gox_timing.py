"""GOX timing limits shared by the Camera workbench and config writeback.

Mirrors gox_driver/include/app_config.h (kMaxFrameRateHz, kMaxExposureMs,
ExposureFitsPeriod); the C++ loader stays the authority at acquisition start.
"""

from __future__ import annotations

import math

# GOX-12405C-PGE, full frame 4128 x 3008, BayerRG12Packed.
MAX_FRAME_RATE_HZ = 6.054  # AcquisitionFrameRate ceiling set by the operator (2026-11-08)
# Focus-assist preview stream only (BayerRG8, kMaxFocusFrameRateHz; operator, 2026-11-16).
MAX_FOCUS_FRAME_RATE_HZ = 9.079
MIN_FRAME_RATE_HZ = 0.125  # longest frame period 8 s (manual p.36/p.141)
MAX_EXPOSURE_MS = 399.405  # ExposureTime maximum 399405 us read back from the camera
MIN_EXPOSURE_MS = 0.001
PERIOD_FRACTION = 0.9  # the exposure may use at most 90 % of the frame period
RATE_STEP_HZ = 0.001


def exposure_limit_ms(frame_rate_hz: float) -> float:
    """Longest exposure that fits ``frame_rate_hz``: 0.9 * 1000 / rate, ms."""
    return PERIOD_FRACTION * 1000.0 / frame_rate_hz


def exposure_fits(exposure_ms: float, frame_rate_hz: float) -> bool:
    """Inclusive, with the same 1e-9 relative allowance as the C++ check."""
    return exposure_ms <= exposure_limit_ms(frame_rate_hz) * (1.0 + 1e-9)


def max_frame_rate_hz(exposure_ms: float, ceiling_hz: float = MAX_FRAME_RATE_HZ) -> float:
    """Highest frame rate, floored to 0.001 Hz and at most ``ceiling_hz``, at which ``exposure_ms``
    uses <= 90 % of the period. The focus-assist stream passes MAX_FOCUS_FRAME_RATE_HZ."""
    exposure_ms = float(exposure_ms)
    if not math.isfinite(exposure_ms) or exposure_ms <= 0:
        raise ValueError("exposure_ms must be finite and positive")
    rate = min(ceiling_hz, PERIOD_FRACTION * 1000.0 / exposure_ms)
    rate = math.floor(rate / RATE_STEP_HZ + 1e-9) * RATE_STEP_HZ
    # Flooring only lowers the rate, so the exposure still fits; guard float edges anyway.
    while rate > MIN_FRAME_RATE_HZ and not exposure_fits(exposure_ms, rate):
        rate -= RATE_STEP_HZ
    return round(max(rate, MIN_FRAME_RATE_HZ), 3)


def check_frame_rate(frame_rate_hz: float) -> float:
    rate = float(frame_rate_hz)
    if not math.isfinite(rate) or not MIN_FRAME_RATE_HZ <= rate <= MAX_FRAME_RATE_HZ:
        raise ValueError(f"frame_rate_hz must be within [{MIN_FRAME_RATE_HZ:g}, {MAX_FRAME_RATE_HZ:g}] Hz")
    return rate
