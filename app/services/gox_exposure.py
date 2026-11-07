"""GOX ETTR exposure series: bracket captures, parallel chart analysis and the exposure fit.

The camera is exclusive, so bracket exposures are captured one after another;
each capture's chart detection, sampling and color calibration run in a worker
thread while the next exposure is captured. A capture whose chart could not be
detected reuses the geometry detected in another capture of the same static,
downward-looking scene. Series sessions stay under the output directory's
``tmp`` folder; nothing here edits a configuration.

Only the exposure is varied or suggested. The gain is whatever the snapshot
configuration sets (x1 in the field); it is read back from each capture for the
fit and is never overridden, raised or recommended.
"""

from __future__ import annotations

import asyncio
import dataclasses
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from ..decoder.ebus import recording as raw_recording
from ..decoder.gox import color_checker, exposure
from ..decoder.gox.color import frame_exposure_from_device
from . import atomic_json, camera_operations, config_store, gox_reference, gox_tools

MAX_SERIES_BYTES = 4 * 1024 * 1024

_status = ""
_series: ExposureSeries | None = None


@dataclass(frozen=True)
class BracketPoint:
    requested_exposure_ms: float
    session: str  # camera directory relative to the series directory
    point: exposure.ExposurePoint | None
    loss: float | None  # RMS CIEDE2000 when the chart could also be color-fitted
    geometry: str
    error: str = ""

    def to_dict(self) -> dict:
        return {"requested_exposure_ms": self.requested_exposure_ms, "session": self.session,
                "point": self.point.to_dict() if self.point else None, "loss_de2000_rms": self.loss,
                "geometry": self.geometry, "error": self.error or None}


@dataclass(frozen=True)
class ExposureCheck:
    """A normal reference capture compared with the ETTR target of the active series."""

    point: exposure.ExposurePoint
    target_dn: float
    deviation: float | None  # None when the patch clipped (only a lower bound is known)
    within: bool
    suggestion: exposure.Suggestion | None  # refined suggestion when not within tolerance
    message: str

    def to_dict(self) -> dict:
        return {"point": self.point.to_dict(), "target_dn": self.target_dn, "deviation": self.deviation,
                "within_tolerance": self.within, "tolerance": exposure.TOLERANCE,
                "suggestion": self.suggestion.to_dict() if self.suggestion else None, "message": self.message}


@dataclass(frozen=True)
class ExposureSeries:
    ok: bool
    reason: str = ""
    chart_key: str = ""
    directory: str = ""
    center_ms: float = 0.0
    gain: float = 1.0  # Gain readback of the bracket captures (unchanged config gain)
    camera_ip: str = ""
    device_id: str = ""
    exposure_offset_us: float = 0.0
    patch: int | None = None
    points: tuple[BracketPoint, ...] = ()
    checks: tuple[ExposureCheck, ...] = ()
    fit: exposure.ExposureFit | None = None
    fit_error: str = ""
    limit_ms: float | None = None  # exposure ceiling of the configs an Apply writes
    limit_notes: tuple[str, ...] = ()
    initial_suggestion: exposure.Suggestion | None = None
    elapsed_s: float = 0.0
    raw_output: str = ""

    @property
    def suggestion(self) -> exposure.Suggestion | None:
        if self.checks and self.checks[-1].suggestion is not None:
            return self.checks[-1].suggestion
        if self.checks and self.checks[-1].within:
            return None  # verified; nothing left to apply
        return self.initial_suggestion

    @property
    def suggested_exposure_ms(self) -> float | None:
        return self.suggestion.exposure_ms if self.suggestion else None

    def to_dict(self) -> dict:
        return {
            "format": "gox-exposure-series-v1", "ok": self.ok, "reason": self.reason or None,
            "chart_type": self.chart_key, "center_exposure_ms": self.center_ms, "gain": self.gain,
            "camera_ip": self.camera_ip, "device_id": self.device_id,
            "exposure_offset_us": self.exposure_offset_us, "patch": self.patch,
            "points": [point.to_dict() for point in self.points],
            "checks": [check.to_dict() for check in self.checks],
            "fit": self.fit.to_dict() if self.fit else None, "fit_error": self.fit_error or None,
            "exposure_limit_ms": self.limit_ms, "exposure_limit_sources": list(self.limit_notes),
            "initial_suggestion": self.initial_suggestion.to_dict() if self.initial_suggestion else None,
            "suggestion": self.suggestion.to_dict() if self.suggestion else None,
            "target_fraction": exposure.TARGET_FRACTION, "tolerance": exposure.TOLERANCE,
            "scope": "exposure estimation only; configs are changed only by an explicit Apply",
        }


def last_series() -> ExposureSeries | None:
    return _series


def status() -> str:
    return _status


@dataclass
class _Analysis:
    camera_dir: Path
    requested_exposure_ms: float
    means: np.ndarray | None = None
    patch_status: list | None = None
    polygons: np.ndarray | None = None
    geometry: str = ""
    full_scale: int = 4095
    exposure_time_us: float | None = None
    gain: float | None = None
    device_id: str = ""
    camera_ip: str = ""
    exposure_offset_us: float = 0.0
    loss: float | None = None
    error: str = ""


def _analyze(camera_dir: Path, requested_ms: float, chart_key: str,
             polygons: np.ndarray | None = None) -> _Analysis:
    """Measure one bracket capture; color-fit it when no patch saturates. Runs in a worker thread."""
    result = _Analysis(camera_dir, requested_ms)
    try:
        device = raw_recording.device_metadata(camera_dir)
        frame_exposure = frame_exposure_from_device(device)
        result.exposure_time_us, result.gain = frame_exposure.exposure_time_us, frame_exposure.gain
        result.device_id = frame_exposure.device_id
        result.camera_ip = str(device.get("ip") or "")
        result.exposure_offset_us = float((device.get("derived") or {}).get("exposure_offset_us") or 0.0)
        measurement = color_checker.measure(camera_dir, chart_key, polygons=polygons)
    except (OSError, ValueError, KeyError, TypeError, cv2.error) as error:
        result.error = str(error) or type(error).__name__
        return result
    result.means, result.patch_status = measurement.means, measurement.status
    result.polygons, result.geometry = measurement.polygons, measurement.geometry
    result.full_scale = measurement.full_scale
    if all(state is None for state in measurement.status):
        try:
            result.loss = color_checker.calibrate(camera_dir, chart_key, measurement=measurement).loss
        except (OSError, ValueError, cv2.error) as error:
            result.error = f"color fit: {error}"
    return result


def _prepare_series(chart_key: str, exposures: list[float]):
    root = gox_reference.reference_tmp_root()
    raw = gox_reference._config_bytes()
    now = datetime.now(timezone.utc)
    directory = root / f"exposure_gox_{now:%Y%m%dT%H%M%SZ}_{uuid.uuid4().hex[:8]}"
    config = directory / "config" / "config-gox-snapshot.yaml"
    directory.mkdir(parents=True, exist_ok=False)
    config.parent.mkdir()
    config.write_bytes(raw)
    atomic_json.write(directory / "series.json", {
        "format": "gox-exposure-series-v1", "status": "capturing", "created_utc": now.isoformat(),
        "chart_type": chart_key, "exposures_ms": exposures, "gain": "unchanged (snapshot config)",
        "config_snapshot": config.relative_to(directory).as_posix(),
    }, limit=MAX_SERIES_BYTES)
    return directory, config


async def run(chart_key: str, center_ms: float) -> ExposureSeries:
    """Bracket the exposure only; the gain stays whatever the snapshot config sets (x1 in the field)."""
    gox_reference._validate_chart(chart_key)
    exposures = exposure.bracket(center_ms)
    if reason := gox_reference.capability_reason():
        raise ValueError(reason)
    return await camera_operations.run(
        "gox", lambda: _run(chart_key, float(center_ms), exposures), kind="gox-exposure-series")


def _point(analysis: _Analysis, patch: int, directory: Path) -> exposure.ExposurePoint:
    return exposure.point_from_means(
        f"bracket {analysis.requested_exposure_ms:g} ms", analysis.means, analysis.patch_status, patch,
        analysis.exposure_time_us, analysis.gain, analysis.full_scale,
        requested_exposure_ms=analysis.requested_exposure_ms,
        source=analysis.camera_dir.relative_to(directory).as_posix())


async def _run(chart_key: str, center_ms: float, exposures: list[float]) -> ExposureSeries:
    global _status, _series
    started = time.monotonic()
    _series = None
    directory = None
    gain = None
    raw_outputs = []
    try:
        _status = "Preparing the exposure series…"
        directory, config = await asyncio.to_thread(_prepare_series, chart_key, exposures)
        pending: list[asyncio.Task | _Analysis] = []
        for index, exposure_ms in enumerate(exposures, 1):
            _status = f"Capturing exposure {index}/{len(exposures)}: {exposure_ms:g} ms…"
            output = directory / f"exposure_{index}_{exposure_ms:g}ms" / "raw" / "gox"
            captured = await gox_tools.capture_raw(None, output, config=config, exposure_ms=exposure_ms)
            raw_outputs.append(f"--- {exposure_ms:g} ms ---\n{captured.raw_output}")
            if not captured.ok or captured.camera_dir is None:
                pending.append(_Analysis(output, exposure_ms, error=captured.reason or "capture failed"))
                continue
            # Analysis overlaps the next capture; the camera itself is never shared.
            pending.append(asyncio.create_task(
                asyncio.to_thread(_analyze, captured.camera_dir, exposure_ms, chart_key)))
        _status = "Analyzing the exposure series…"
        analyses = [item if isinstance(item, _Analysis) else await item for item in pending]
        geometry = next((item.polygons for item in analyses if item.polygons is not None), None)
        if geometry is not None:
            for index, item in enumerate(analyses):
                if item.means is None and item.camera_dir.is_dir() and (item.camera_dir / "device.json").exists():
                    analyses[index] = await asyncio.to_thread(
                        _analyze, item.camera_dir, item.requested_exposure_ms, chart_key, geometry)
        measured = [item for item in analyses if item.means is not None]
        if not measured:
            reasons = "; ".join(f"{item.requested_exposure_ms:g} ms: {item.error}" for item in analyses)
            raise ValueError(f"No exposure produced a usable chart measurement ({reasons})")
        gains = {item.gain for item in measured}
        if len(gains) != 1:
            raise ValueError(f"The bracket captures report different gains {sorted(gains)}; "
                             "the series varies the exposure only")
        gain = gains.pop()
        lowest = min(measured, key=lambda item: item.exposure_time_us)
        patch = exposure.brightest_patch(lowest.means)
        points = tuple(BracketPoint(
            item.requested_exposure_ms, item.camera_dir.relative_to(directory).as_posix(),
            _point(item, patch, directory) if item.means is not None else None,
            item.loss, item.geometry, item.error) for item in analyses)
        fit, fit_error = None, ""
        try:
            fit = exposure.fit([point.point for point in points if point.point is not None],
                               gain=gain, exposure_offset_us=lowest.exposure_offset_us,
                               full_scale=lowest.full_scale)
        except ValueError as error:
            fit_error = str(error)
        limit_ms, limit_notes = await asyncio.to_thread(_exposure_limit, lowest.camera_ip)
        suggestion = None
        if fit is not None:
            try:
                suggestion = exposure.suggest(fit.target_signal, gain=gain,
                                              exposure_offset_us=lowest.exposure_offset_us, limit_ms=limit_ms,
                                              fit=fit)
            except ValueError as error:
                fit_error = str(error)
        series = ExposureSeries(
            ok=suggestion is not None, reason=fit_error, chart_key=chart_key, directory=str(directory),
            center_ms=center_ms, gain=gain, camera_ip=lowest.camera_ip, device_id=lowest.device_id,
            exposure_offset_us=lowest.exposure_offset_us, patch=patch, points=points, fit=fit,
            fit_error=fit_error, limit_ms=limit_ms, limit_notes=tuple(limit_notes),
            initial_suggestion=suggestion, elapsed_s=time.monotonic() - started,
            raw_output="\n".join(raw_outputs))
    except asyncio.CancelledError:
        _status = "Exposure series interrupted; captured RAW is retained"
        raise
    except Exception as error:
        series = ExposureSeries(ok=False, reason=str(error) or type(error).__name__, chart_key=chart_key,
                                directory=str(directory) if directory else "", center_ms=center_ms,
                                gain=gain if gain is not None else 1.0,
                                elapsed_s=time.monotonic() - started, raw_output="\n".join(raw_outputs))
    await _save(series)
    _series = series
    _status = (f"Suggested exposure {series.suggestion.exposure_ms:g} ms (ETTR)"
               if series.ok
               else f"Exposure series failed: {series.reason}")
    return series


def _exposure_limit(camera_ip: str) -> tuple[float | None, list[str]]:
    if not camera_ip:
        return None, ["camera address unknown; the frame-period limit is checked on Apply"]
    try:
        return config_store.gox_exposure_limit_ms(camera_ip)
    except (OSError, ValueError, config_store.ApplyError) as error:
        return None, [f"frame-period limit unavailable ({error}); it is checked on Apply"]


async def _save(series: ExposureSeries) -> None:
    if series.directory:
        document = series.to_dict()
        document["status"] = "completed" if series.ok else "failed"
        try:
            await asyncio.to_thread(atomic_json.write, Path(series.directory) / "series.json", document,
                                    limit=MAX_SERIES_BYTES)
        except (OSError, ValueError):
            pass  # the in-memory result still reports the outcome


# --- verification of a normal reference capture ----------------------------------

def _applicable(series: ExposureSeries | None, chart_key: str, device_id: str | None) -> bool:
    return (series is not None and series.ok and series.fit is not None and series.patch is not None
            and series.chart_key == chart_key and bool(device_id) and device_id == series.device_id)


def _refit(series: ExposureSeries, extra: exposure.ExposurePoint) -> tuple[exposure.Suggestion | None, str]:
    points = [point.point for point in series.points if point.point is not None]
    points += [check.point for check in series.checks] + [extra]
    try:
        suggestion, prefix = exposure.refine(series.fit, points, extra, exposure_offset_us=series.exposure_offset_us,
                                             limit_ms=series.limit_ms)
    except ValueError as error:
        return None, f"Refit failed: {error}"
    note = f" {suggestion.note}" if suggestion.note else ""
    return suggestion, f"{prefix} {suggestion.exposure_ms:g} ms; apply it and capture again.{note}"


def _reachable_target(series: ExposureSeries, gain: float) -> tuple[float, str]:
    """The ETTR target, or the peak the frame-period ceiling allows when that is lower."""
    target, ceiling_ms = exposure.reachable_target(series.fit, gain=gain, exposure_offset_us=series.exposure_offset_us,
                                                   limit_ms=series.limit_ms)
    if ceiling_ms is None:
        return target, ""
    return target, (f" The {series.limit_ms:g} ms frame-period limit caps the exposure at {ceiling_ms:g} ms, "
                    f"so the reachable peak ({target:.0f} DN) is the target instead of {series.fit.target_dn:.0f} DN.")


def check_reference(chart_key: str, provenance: dict, source: str,
                    status: list[str | None] | None = None,
                    means: np.ndarray | None = None) -> ExposureCheck | None:
    """Compare a reference capture with the active series' ETTR target; None if not applicable.

    ``means``/``status`` default to the calibration provenance (an unsaturated capture).
    """
    global _series
    series = _series
    reference = provenance.get("reference_exposure") or {}
    if not _applicable(series, chart_key, reference.get("device_id")):
        return None
    if means is None:
        means = np.asarray(provenance["patch_rgb_normalized"], dtype=np.float64)
    status = status if status is not None else [None] * len(means)
    point = exposure.point_from_means("verification", means, status, series.patch,
                                      reference["exposure_time_us"], reference["gain"],
                                      int(provenance.get("source_full_scale_dn", 4095)), source=source)
    target, reach_note = _reachable_target(series, point.gain)
    if not point.usable:
        suggested, message = _refit(series, point)
        check = ExposureCheck(point, target, None, False, suggested, message)
    else:
        deviation = exposure.deviation(point.peak_dn, target)
        within = abs(deviation) <= exposure.TOLERANCE
        suggested, message = (None, f"Within ±{exposure.TOLERANCE:.0%} of the ETTR target.{reach_note}") \
            if within else _refit(series, point)
        check = ExposureCheck(point, target, deviation, within, suggested, message)
    _series = dataclasses.replace(series, checks=series.checks + (check,))
    return check


def check_failed_capture(chart_key: str, camera_dir: Path, source: str) -> ExposureCheck | None:
    """A reference whose calibration failed: measure saturation for the exposure check."""
    series = _series
    if series is None or not series.ok:
        return None
    try:
        device = raw_recording.device_metadata(camera_dir)
        frame_exposure = frame_exposure_from_device(device)
        measurement = color_checker.measure(camera_dir, chart_key)
    except (OSError, ValueError, KeyError, TypeError, cv2.error):
        return None
    provenance = {"reference_exposure": frame_exposure.to_dict(), "source_full_scale_dn": measurement.full_scale}
    return check_reference(chart_key, provenance, source, measurement.status, measurement.means)


async def save_current() -> None:
    if _series is not None:
        await _save(_series)
