"""ETTR exposure estimation from a ColorChecker exposure series; pure arithmetic.

The camera is linear (LUT/gradation off, audited), so the chart's brightest
patch follows ``peak_dn = offset + slope * (t * g)``: ``t`` is the actual
integration time (ExposureTime readback + documented offset), ``g`` the
Gain[AnalogAll] magnification, ``offset`` the black level in DN. Scene flare
scales with exposure, so it enters the slope, not the offset. Only patches
without clipped photosites and below the near-saturation limit are fitted;
a clipped patch's mean is merely a lower bound of its signal.

Exposure is the only control suggested; the gain enters the model as the
measured readback and is never changed (field captures keep gain x1).
"""

from __future__ import annotations

from dataclasses import dataclass, field
import math

import numpy as np

TARGET_FRACTION = 0.80  # ETTR: brightest patch peak channel at 80 % of full scale
TOLERANCE = 0.05  # accepted relative deviation of a verification capture
BRACKET_STEP_MS = 50.0
EXTRAPOLATION_WARNING = 2.0
_CHANNELS = "RGB"


def bracket(center_ms: float, step_ms: float = BRACKET_STEP_MS) -> list[float]:
    """center ± step (50 ms for GOX); when center <= step the low step is center / 2."""
    if isinstance(center_ms, bool) or not isinstance(center_ms, (int, float)) or not math.isfinite(center_ms) \
            or center_ms <= 0:
        raise ValueError("The initial exposure must be a finite positive number of milliseconds")
    center = float(center_ms)
    low = center - step_ms if center > step_ms else center / 2.0
    return [round(low, 3), round(center, 3), round(center + step_ms, 3)]


def brightest_patch(means: np.ndarray) -> int:
    """1-based index of the brightest patch (MCC24: #19 white) of one capture."""
    means = np.asarray(means, dtype=np.float64)
    if means.ndim != 2 or means.shape[1] != 3 or means.size == 0:
        raise ValueError("Patch means must be an (N, 3) array")
    return int(np.argmax(means.sum(axis=1))) + 1


@dataclass(frozen=True)
class ExposurePoint:
    label: str
    exposure_time_us: float  # actual integration time
    gain: float
    patch: int  # 1-based
    peak_dn: float  # brightest channel mean of the patch, DN
    peak_channel: str
    status: str | None  # None, "clipped" or "near_saturation"
    requested_exposure_ms: float | None = None
    source: str = ""

    @property
    def signal(self) -> float:
        return self.exposure_time_us * self.gain

    @property
    def usable(self) -> bool:
        return self.status is None

    def to_dict(self) -> dict:
        return {"label": self.label, "requested_exposure_ms": self.requested_exposure_ms,
                "exposure_time_us": self.exposure_time_us, "gain": self.gain, "patch": self.patch,
                "peak_dn": self.peak_dn, "peak_channel": self.peak_channel, "status": self.status,
                "source": self.source}


def point_from_means(label: str, means: np.ndarray, status: list[str | None], patch: int,
                     exposure_time_us: float, gain: float, full_scale: int, *,
                     requested_exposure_ms: float | None = None, source: str = "") -> ExposurePoint:
    means = np.asarray(means, dtype=np.float64)
    if not 1 <= patch <= len(means) or len(status) != len(means):
        raise ValueError("Patch index or status does not match the measured chart")
    values = means[patch - 1] * full_scale
    channel = int(np.argmax(values))
    return ExposurePoint(label, float(exposure_time_us), float(gain), patch, float(values[channel]),
                         _CHANNELS[channel], status[patch - 1], requested_exposure_ms, source)


@dataclass(frozen=True)
class ExposureFit:
    target_dn: float
    target_fraction: float
    tolerance: float
    offset_dn: float
    slope_dn_per_us: float  # per microsecond of integration at gain x1
    residual_rms_dn: float
    gain: float
    exposure_offset_us: float
    suggested_exposure_ms: float
    points_used: int
    points_excluded: int
    warnings: list[str] = field(default_factory=list)

    def predicted_dn(self, exposure_time_us: float, gain: float) -> float:
        return self.offset_dn + self.slope_dn_per_us * exposure_time_us * gain

    @property
    def target_signal(self) -> float:
        """Integration time × gain (µs) that puts the patch on the target."""
        return (self.target_dn - self.offset_dn) / self.slope_dn_per_us

    def to_dict(self) -> dict:
        return {
            "model": "peak_dn = offset_dn + slope_dn_per_us * actual_integration_us * gain",
            "target_dn": self.target_dn, "target_fraction": self.target_fraction,
            "tolerance": self.tolerance, "offset_dn": self.offset_dn,
            "slope_dn_per_us": self.slope_dn_per_us, "residual_rms_dn": self.residual_rms_dn,
            "gain": self.gain, "exposure_offset_us": self.exposure_offset_us,
            "suggested_exposure_ms": self.suggested_exposure_ms,
            "points_used": self.points_used, "points_excluded": self.points_excluded,
            "warnings": list(self.warnings),
        }


def fit(points: list[ExposurePoint], *, gain: float, exposure_offset_us: float, full_scale: int = 4095,
        target_fraction: float = TARGET_FRACTION, tolerance: float = TOLERANCE) -> ExposureFit:
    """Least-squares line through unsaturated points; solve for the ETTR exposure at ``gain``."""
    if not math.isfinite(gain) or gain <= 0 or not math.isfinite(exposure_offset_us) or exposure_offset_us < 0:
        raise ValueError("Exposure fit requires a positive gain and a nonnegative exposure offset")
    if len({point.patch for point in points}) > 1:
        raise ValueError("Exposure points must measure the same chart patch")
    usable = [point for point in points if point.usable]
    clipped = [point for point in points if not point.usable]
    if len({point.signal for point in usable}) < 2:
        if points and not usable:
            raise ValueError("Every exposure saturates the brightest patch; lower the initial exposure")
        if clipped:
            raise ValueError("Only one exposure is below saturation; lower the initial exposure and retry")
        raise ValueError("At least two different unsaturated exposures are required for the fit")
    x = np.array([point.signal for point in usable], dtype=np.float64)
    y = np.array([point.peak_dn for point in usable], dtype=np.float64)
    slope, offset = np.polyfit(x, y, 1)
    # Floating-point noise gives a flat response a tiny nonzero slope; require a real DN change.
    if not math.isfinite(slope) or slope * float(x.max() - x.min()) < 1.0:
        raise ValueError("The brightest patch does not brighten with exposure; check the scene and recapture")
    residual = float(np.sqrt(np.mean((y - (offset + slope * x)) ** 2)))
    target = target_fraction * full_scale
    signal = (target - offset) / slope
    if signal <= 0:
        raise ValueError("The fitted black offset is above the ETTR target; check the measurements")
    exposure_us = signal / gain
    exposure_ms = (exposure_us - exposure_offset_us) / 1000.0
    if exposure_ms <= 0:
        raise ValueError("The fitted exposure is not positive; check the measurements")
    warnings = []
    ratio = signal / float(x.max())
    if ratio > EXTRAPOLATION_WARNING:
        warnings.append(f"Extrapolated ×{ratio:.1f} beyond the brightest unsaturated exposure; "
                        "the verification capture confirms it.")
    if clipped and signal >= min(point.signal for point in clipped):
        warnings.append("A shorter or equal exposure already saturated the patch; the series is inconsistent.")
    if not 0 <= offset < 0.5 * full_scale:
        warnings.append(f"Unusual fitted black offset {offset:.0f} DN.")
    if residual > tolerance * target / 2:
        warnings.append(f"Large fit residual ({residual:.0f} DN RMS); the response may not be linear.")
    return ExposureFit(float(target), float(target_fraction), float(tolerance), float(offset), float(slope), residual,
                       float(gain), float(exposure_offset_us), round(float(exposure_ms), 1),
                       len(usable), len(clipped), warnings)


LIMIT_MARGIN = 0.98  # stay this fraction below a configuration's exposure ceiling


@dataclass(frozen=True)
class Suggestion:
    """An exposure to apply. The gain is never changed: it is the gain the fit
    was evaluated at (the camera's configured gain, x1 in the field)."""
    exposure_ms: float
    gain: float
    note: str = ""
    expected_dn: float | None = None  # predicted patch peak at this exposure
    capped: bool = False  # the frame-period ceiling, not the ETTR target, set the exposure

    def to_dict(self) -> dict:
        return {"exposure_ms": self.exposure_ms, "gain": self.gain, "note": self.note or None,
                "expected_dn": self.expected_dn, "capped_by_frame_period": self.capped}


def capped_exposure_ms(limit_ms: float) -> float:
    """Longest exposure suggested under a configuration's ``limit_ms`` ceiling."""
    return math.floor(limit_ms * LIMIT_MARGIN * 10.0) / 10.0


def suggest(target_signal: float, *, gain: float, exposure_offset_us: float,
            limit_ms: float | None = None, fit: ExposureFit | None = None,
            unchanged: str | None = None) -> Suggestion:
    """Exposure for ``target_signal`` (µs × gain) at the unchanged ``gain``.

    Above ``limit_ms`` the exposure is capped just below the ceiling and the
    patch lands under the ETTR target; the gain is never raised to compensate.
    ``fit`` (optional) adds the predicted patch peak to the suggestion;
    ``unchanged`` names the settings kept as they are (default: the gain).
    """
    if not math.isfinite(target_signal) or target_signal <= 0:
        raise ValueError("The target signal must be positive")
    if not math.isfinite(gain) or gain <= 0:
        raise ValueError("The gain must be positive")
    exposure_ms = round((target_signal / gain - exposure_offset_us) / 1000.0, 1)
    if exposure_ms <= 0:
        raise ValueError("The ETTR exposure is not positive; the chart is already brighter than the target")

    def expected(ms: float) -> float | None:
        return fit.predicted_dn(ms * 1000.0 + exposure_offset_us, gain) if fit is not None else None

    if limit_ms is None or exposure_ms < limit_ms * LIMIT_MARGIN:
        return Suggestion(exposure_ms, float(gain), expected_dn=expected(exposure_ms))
    capped = capped_exposure_ms(limit_ms)
    if capped <= 0:
        raise ValueError(f"The {limit_ms:g} ms frame-period limit leaves no usable exposure")
    peak = expected(capped)
    reach = f" (patch ≈ {peak:.0f} DN, {peak * fit.target_fraction / fit.target_dn:.0%} FS)" if peak is not None else ""
    return Suggestion(capped, float(gain),
                      f"The ETTR exposure {exposure_ms:g} ms exceeds the {limit_ms:g} ms frame-period limit; "
                      f"capped at {capped:g} ms{reach}, below the ETTR target. "
                      f"{unchanged if unchanged is not None else f'Gain stays x{gain:g}.'}",
                      expected_dn=peak, capped=True)


def full_scale_of(fit: ExposureFit) -> float:
    return fit.target_dn / fit.target_fraction


def reachable_target(fit: ExposureFit, *, gain: float, exposure_offset_us: float,
                     limit_ms: float | None) -> tuple[float, float | None]:
    """The ETTR target DN, or the lower peak the frame-period ceiling allows.

    Returns (target DN, capped exposure in ms when the ceiling binds, else None).
    """
    if limit_ms is None:
        return fit.target_dn, None
    ceiling_ms = capped_exposure_ms(limit_ms)
    peak = fit.predicted_dn(ceiling_ms * 1000.0 + exposure_offset_us, gain)
    return (fit.target_dn, None) if peak >= fit.target_dn else (peak, ceiling_ms)


def refine(base: ExposureFit, points: list[ExposurePoint], extra: ExposurePoint, *, exposure_offset_us: float,
           limit_ms: float | None, unchanged: str | None = None) -> tuple[Suggestion, str]:
    """Suggestion after a verification capture ``extra`` (already in ``points``).

    An unsaturated capture is refitted with all points at the series' target
    fraction; a saturated one only bounds the response from below, so it gives
    a conservative exposure from the original fit's offset. Returns the
    suggestion and a message prefix.
    """
    full_scale = full_scale_of(base)
    if not extra.usable:
        signal = extra.signal * (base.target_dn - base.offset_dn) / (full_scale - base.offset_dn)
        suggestion = suggest(signal, gain=extra.gain, exposure_offset_us=exposure_offset_us, limit_ms=limit_ms,
                             unchanged=unchanged)
        return suggestion, "The brightest measurement saturated; at most"
    refit = fit(points, gain=extra.gain, exposure_offset_us=exposure_offset_us,
                full_scale=int(round(full_scale)), target_fraction=base.target_fraction, tolerance=base.tolerance)
    suggestion = suggest(refit.target_signal, gain=extra.gain, exposure_offset_us=exposure_offset_us,
                         limit_ms=limit_ms, fit=refit, unchanged=unchanged)
    return suggestion, "Refined with this capture:"


def deviation(peak_dn: float, target_dn: float) -> float:
    """Relative deviation of a measured peak from the ETTR target."""
    return (float(peak_dn) - float(target_dn)) / float(target_dn)
