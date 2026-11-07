"""FX10 white-reference ETTR measurements; pure arithmetic on recorded spectra.

The measured quantity is the per-band p90 DN over every frame and spatial
sample of a white-target capture (the statistic the Reference page plots); the
brightest band is chosen at the lowest bracket exposure and kept for the whole
series. The fit and suggestion arithmetic is shared with GOX
(``app.decoder.gox.exposure``): ``p90_dn = offset + slope * t``. The FX10 has no
gain; only the exposure is varied or suggested, never the binning.
"""

from __future__ import annotations

import math

from ..gox import exposure as shared

BRACKET_STEP_MS = 15.0  # hyperspectral response is exposure sensitive: a narrow bracket
DEFAULT_TARGET_FRACTION = 0.60
MIN_TARGET_FRACTION, MAX_TARGET_FRACTION = 0.10, 0.95
NEAR_SATURATION = 0.98  # p90 at or above this fraction of full scale is not fitted
STATISTIC = "p90"
UNCHANGED = "Binning is unchanged."


def bracket(center_ms: float) -> list[float]:
    """center ± 15 ms; when center <= 15 ms the low step is center / 2."""
    return shared.bracket(center_ms, BRACKET_STEP_MS)


def target_fraction(value: object) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) \
            or not MIN_TARGET_FRACTION <= value <= MAX_TARGET_FRACTION:
        raise ValueError(f"The ETTR target must be within {MIN_TARGET_FRACTION:.0%} – "
                         f"{MAX_TARGET_FRACTION:.0%} of full scale")
    return float(value)


def brightest_band(p90: list[float]) -> int:
    """1-based index of the band with the highest p90."""
    if not p90 or not all(math.isfinite(float(value)) for value in p90):
        raise ValueError("The spectrum has no finite band statistics")
    return max(range(len(p90)), key=lambda index: float(p90[index])) + 1


def band_status(value_dn: float, full_scale: int) -> str | None:
    if value_dn >= full_scale:
        return "clipped"
    if value_dn >= NEAR_SATURATION * full_scale:
        return "near_saturation"
    return None


def band_label(band: int, wavelengths_nm: list[float] | tuple[float, ...]) -> str:
    if wavelengths_nm and 1 <= band <= len(wavelengths_nm):
        return f"{STATISTIC} band {band} ({float(wavelengths_nm[band - 1]):.1f} nm)"
    return f"{STATISTIC} band {band}"


def point_from_spectrum(label: str, p90: list[float], band: int, exposure_time_us: float, full_scale: int,
                        wavelengths_nm: list[float] | tuple[float, ...] = (), *,
                        requested_exposure_ms: float | None = None, source: str = "") -> shared.ExposurePoint:
    if not 1 <= band <= len(p90):
        raise ValueError("Band index does not match the measured spectrum")
    value = float(p90[band - 1])
    return shared.ExposurePoint(label, float(exposure_time_us), 1.0, band, value, band_label(band, wavelengths_nm),
                                band_status(value, full_scale), requested_exposure_ms, source)


def _readback(device: dict, group: str, name: str):
    entry = (device.get(group) or {}).get(name) or {}
    if entry.get("status") != "ok" or entry.get("value") is None:
        raise ValueError(f"device.json has no verified {group}.{name} readback")
    return entry["value"]


def exposure_from_device(device: dict) -> dict:
    """Identity, ExposureTime (µs) and binning readbacks of one FX10 capture; nothing is defaulted."""
    if device.get("kind") != "fx10":
        raise ValueError("Recording is not an FX10 capture")
    exposure_us = _readback(device, "final_readback", "ExposureTime")
    if isinstance(exposure_us, bool) or not isinstance(exposure_us, (int, float)) or not math.isfinite(exposure_us) \
            or exposure_us <= 0:
        raise ValueError("device.json ExposureTime readback is not a positive number")
    return {
        "device_id": str(_readback(device, "identity", "DeviceSerialNumber")),
        "device_model": str(_readback(device, "identity", "DeviceModelName")),
        "exposure_time_us": float(exposure_us),
        "spatial_binning": int(_readback(device, "final_readback", "BinningHorizontal")),
        "spectral_binning": int(_readback(device, "final_readback", "BinningVertical")),
    }
