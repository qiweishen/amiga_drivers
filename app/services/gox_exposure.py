"""GOX ETTR exposure series: bracket captures, the operator's chart selection and the exposure fit.

The camera is exclusive, so bracket exposures are captured one after another;
each capture's chart detection runs in a worker thread while the next exposure
is captured. The detector's candidates from every capture are then offered to
the operator as thumbnails (``choose`` / ``reject``); the selected geometry
measures all captures of the same static, downward-looking scene. Series
sessions stay under the output directory's ``tmp`` folder; nothing here edits
a configuration.

Only the exposure is varied or suggested. The gain is whatever the snapshot
configuration sets (x1 in the field); it is read back from each capture for the
fit and is never overridden, raised or recommended.
"""

from __future__ import annotations

import asyncio
import dataclasses
import time
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

from ..decoder.ebus import recording as raw_recording
from ..decoder import exposure as ettr
from ..decoder.gox import color_checker, exposure
from ..decoder.gox.color import frame_exposure_from_device
from . import camera_operations, config_store, exposure_series, gox_chart, gox_tools, timing
from .exposure_series import ExposureCheck
from ..constants import SNAPSHOT_CONFIG

_status = ""
_series: ExposureSeries | None = None
_pending: _PendingSeries | None = None


@dataclass(frozen=True)
class BracketPoint:
    requested_exposure_ms: float
    session: str  # camera directory relative to the series directory
    point: ettr.ExposurePoint | None
    loss: float | None  # RMS CIEDE2000 when the chart could also be color-fitted
    geometry: str  # color_checker.GEOMETRY_SELECTED on the chosen capture, GEOMETRY_SUPPLIED elsewhere
    error: str = ""

    def to_dict(self) -> dict:
        return {"requested_exposure_ms": self.requested_exposure_ms, "session": self.session,
                "point": self.point.to_dict() if self.point else None, "loss_de2000_rms": self.loss,
                "geometry": self.geometry, "error": self.error or None}


@dataclass(frozen=True)
class ExposureSeries(exposure_series.Suggested):
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
    fit: ettr.ExposureFit | None = None
    fit_error: str = ""
    limit_ms: float | None = None  # exposure ceiling of the configs an Apply writes
    limit_notes: tuple[str, ...] = ()
    initial_suggestion: ettr.Suggestion | None = None
    elapsed_s: float = 0.0
    raw_output: str = ""
    # Detector candidates waiting for the operator (choose / reject); empty otherwise.
    candidates: tuple[gox_chart.CandidateView, ...] = ()
    selection: dict | None = None  # the chart the operator selected for every bracket capture

    @property
    def awaiting_selection(self) -> bool:
        return bool(self.candidates)

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
            "target_fraction": exposure.TARGET_FRACTION, "tolerance": ettr.TOLERANCE,
            "chart_selection": self.selection,
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


def _analyze(camera_dir: Path, requested_ms: float, chart_key: str, polygons: np.ndarray, *,
             geometry: str, detection_transfer: str, selection: dict) -> _Analysis:
    """Measure one bracket capture inside the selected chart; color-fit it when no patch saturates.

    Runs in a worker thread.
    """
    result = _Analysis(camera_dir, requested_ms)
    try:
        device = raw_recording.device_metadata(camera_dir)
        frame_exposure = frame_exposure_from_device(device)
        result.exposure_time_us, result.gain = frame_exposure.exposure_time_us, frame_exposure.gain
        result.device_id = frame_exposure.device_id
        result.camera_ip = str(device.get("ip") or "")
        result.exposure_offset_us = float((device.get("derived") or {}).get("exposure_offset_us") or 0.0)
        measurement = color_checker.measure(camera_dir, chart_key, polygons, geometry=geometry,
                                            detection_transfer=detection_transfer, selection=selection)
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
    return exposure_series.prepare(
        "exposure_gox", "config-gox-snapshot.yaml", config_store.snapshot_config_bytes(SNAPSHOT_CONFIG),
        {"format": "gox-exposure-series-v1", "chart_type": chart_key, "exposures_ms": exposures,
         "gain": "unchanged (snapshot config)"})


async def run(chart_key: str, center_ms: float) -> ExposureSeries:
    """Bracket the exposure only; the gain stays whatever the snapshot config sets (x1 in the field)."""
    gox_chart.check_key(chart_key)
    exposures = exposure.bracket(center_ms)
    if max(exposures) > timing.GOX_MAX_EXPOSURE_MS:
        raise ValueError(f"The bracket reaches {max(exposures):g} ms, above the GOX ExposureTime maximum "
                         f"{timing.GOX_MAX_EXPOSURE_MS:g} ms; lower the initial exposure")
    if reason := color_checker.capability_reason():
        raise ValueError(reason)
    return await camera_operations.run(
        "gox", lambda: _run(chart_key, float(center_ms), exposures), kind="gox-exposure-series")


def _point(analysis: _Analysis, patch: int, directory: Path) -> ettr.ExposurePoint:
    return exposure.point_from_means(
        f"bracket {analysis.requested_exposure_ms:g} ms", analysis.means, analysis.patch_status, patch,
        analysis.exposure_time_us, analysis.gain, analysis.full_scale,
        requested_exposure_ms=analysis.requested_exposure_ms,
        source=analysis.camera_dir.relative_to(directory).as_posix())


@dataclass
class _Capture:
    """One bracket capture and the chart candidates detected in it."""

    camera_dir: Path
    requested_exposure_ms: float
    captured: bool = True
    candidates: tuple[color_checker.ChartCandidate, ...] = ()
    transfer: str = ""
    error: str = ""


def _detect_capture(camera_dir: Path, requested_ms: float, chart_key: str) -> _Capture:
    """Chart candidates of one bracket capture. Runs in a worker thread."""
    try:
        detection = color_checker.detect(camera_dir, chart_key)
    except (OSError, ValueError, KeyError, TypeError, cv2.error) as error:
        return _Capture(camera_dir, requested_ms, error=str(error) or type(error).__name__)
    return _Capture(camera_dir, requested_ms, candidates=detection.candidates, transfer=detection.transfer)


@dataclass
class _PendingSeries:
    """Bracket captures waiting for the operator's chart selection."""

    chart_key: str
    center_ms: float
    directory: Path
    captures: list[_Capture]
    offered: list[tuple[_Capture, color_checker.ChartCandidate]]  # numbered from 1, as shown
    raw_output: str
    started: float


def _failed(pending: _PendingSeries, reason: str, selection: dict | None = None) -> ExposureSeries:
    return ExposureSeries(ok=False, reason=reason, chart_key=pending.chart_key, directory=str(pending.directory),
                          center_ms=pending.center_ms, elapsed_s=time.monotonic() - pending.started,
                          raw_output=pending.raw_output, selection=selection)


def _take_pending() -> _PendingSeries:
    global _pending
    pending, _pending = _pending, None  # one decision per series, whichever client makes it
    if pending is None:
        raise ValueError("No exposure series is waiting for a chart selection")
    if _series is not None:
        gox_chart.release(_series.candidates)
    return pending


async def _abandon_pending(reason: str) -> None:
    """A series still waiting for a chart selection fails when a new one replaces it."""
    if _pending is not None:
        await _save(_failed(_take_pending(), reason))


async def _run(chart_key: str, center_ms: float, exposures: list[float]) -> ExposureSeries:
    """Capture the bracket and detect; the result waits for the operator's chart selection."""
    global _status, _series, _pending
    started = time.monotonic()
    await _abandon_pending("Replaced by a new exposure series before a chart was selected")
    _series = None
    directory = None
    raw_outputs = []
    extra = None
    try:
        _status = "Preparing the exposure series…"
        directory, config = await asyncio.to_thread(_prepare_series, chart_key, exposures)
        tasks: list[asyncio.Task | _Capture] = []
        for index, exposure_ms in enumerate(exposures, 1):
            _status = f"Capturing exposure {index}/{len(exposures)}: {exposure_ms:g} ms…"
            output = directory / f"exposure_{index}_{exposure_ms:g}ms" / "raw" / "gox"
            # One freerun frame: run each point at the fastest rate its exposure allows (90 % rule).
            captured = await gox_tools.capture_raw(None, output, config=config, exposure_ms=exposure_ms,
                                                   frame_rate_hz=timing.gox_max_frame_rate_hz(exposure_ms))
            raw_outputs.append(f"--- {exposure_ms:g} ms ---\n{captured.raw_output}")
            if not captured.ok or captured.camera_dir is None:
                tasks.append(_Capture(output, exposure_ms, captured=False,
                                      error=captured.reason or "capture failed"))
                continue
            # Detection overlaps the next capture; the camera itself is never shared.
            tasks.append(asyncio.create_task(
                asyncio.to_thread(_detect_capture, captured.camera_dir, exposure_ms, chart_key)))
        _status = "Detecting the chart in the bracket captures…"
        captures = [item if isinstance(item, _Capture) else await item for item in tasks]
        offered = [(capture, candidate) for capture in captures for candidate in capture.candidates]
        if not offered:
            reasons = "; ".join(f"{item.requested_exposure_ms:g} ms: {item.error}" for item in captures)
            raise ValueError(f"No exposure produced a chart candidate ({reasons})")
        raw_output = "\n".join(raw_outputs)
        views = gox_chart.register(directory.name, [
            (f"{capture.requested_exposure_ms:g} ms · candidate {candidate.number}", candidate)
            for capture, candidate in offered])
        _pending = _PendingSeries(chart_key, center_ms, directory, captures, offered, raw_output, started)
        series = ExposureSeries(ok=False, chart_key=chart_key, directory=str(directory), center_ms=center_ms,
                                elapsed_s=time.monotonic() - started, raw_output=raw_output, candidates=views)
        extra = {"chart_candidates": [
            {"number": number, "exposure_ms": capture.requested_exposure_ms,
             "capture": capture.camera_dir.relative_to(directory).as_posix(), **candidate.to_dict()}
            for number, (capture, candidate) in enumerate(offered, 1)]}
        _status = f"{len(views)} chart candidate(s) detected in the bracket; select the chart"
    except asyncio.CancelledError:
        _status = "Exposure series interrupted; captured RAW is retained"
        raise
    except Exception as error:
        series = ExposureSeries(ok=False, reason=str(error) or type(error).__name__, chart_key=chart_key,
                                directory=str(directory) if directory else "", center_ms=center_ms,
                                elapsed_s=time.monotonic() - started, raw_output="\n".join(raw_outputs))
        _status = f"Exposure series failed: {series.reason}"
    await _save(series, extra)
    _series = series
    return series


async def choose(number: int) -> ExposureSeries:
    """Measure every bracket capture with the chart the operator selected, then fit the exposure."""
    global _status, _series
    pending = _pending
    if pending is None:
        raise ValueError("No exposure series is waiting for a chart selection")
    if not isinstance(number, int) or not 1 <= number <= len(pending.offered):
        raise ValueError(f"Candidate {number} was not offered")
    pending = _take_pending()
    source, candidate = pending.offered[number - 1]
    selection = {"candidate": number, "exposure_ms": source.requested_exposure_ms,
                 "capture": source.camera_dir.relative_to(pending.directory).as_posix(),
                 "capture_candidate": candidate.number, "opencv_cost": candidate.cost,
                 "candidate_count": len(pending.offered), "selected_by": "operator"}
    try:
        analyses = []
        for index, capture in enumerate(pending.captures, 1):
            _status = f"Measuring exposure {index}/{len(pending.captures)} with the selected chart…"
            if not capture.captured:
                analyses.append(_Analysis(capture.camera_dir, capture.requested_exposure_ms, error=capture.error))
                continue
            selected = capture is source
            # One capture at a time: each holds a full-resolution native RGB image.
            analyses.append(await asyncio.to_thread(
                _analyze, capture.camera_dir, capture.requested_exposure_ms, pending.chart_key, candidate.polygons,
                geometry=color_checker.GEOMETRY_SELECTED if selected else color_checker.GEOMETRY_SUPPLIED,
                detection_transfer=source.transfer if selected else color_checker.SUPPLIED_TRANSFER,
                selection=selection))
        series = await _finish(pending, analyses, selection)
    except asyncio.CancelledError:
        _status = "Exposure series interrupted; captured RAW is retained"
        raise
    except Exception as error:
        series = _failed(pending, str(error) or type(error).__name__, selection)
    await _save(series)
    _series = series
    _status = (f"Suggested exposure {series.suggestion.exposure_ms:g} ms (ETTR)" if series.ok
               else f"Exposure series failed: {series.reason}")
    return series


async def reject() -> ExposureSeries:
    """None of the candidates is the chart: the series fails and its RAW is kept."""
    global _status, _series
    series = _failed(_take_pending(), gox_chart.NO_CHART_SELECTED)
    await _save(series)
    _series = series
    _status = f"Exposure series failed: {series.reason}"
    return series


async def _finish(pending: _PendingSeries, analyses: list[_Analysis], selection: dict) -> ExposureSeries:
    """The ETTR fit of the measured bracket."""
    directory = pending.directory
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
        fit = ettr.fit([point.point for point in points if point.point is not None],
                       gain=gain, exposure_offset_us=lowest.exposure_offset_us,
                       full_scale=lowest.full_scale, target_fraction=exposure.TARGET_FRACTION)
    except ValueError as error:
        fit_error = str(error)
    limit_ms, limit_notes = await asyncio.to_thread(_exposure_limit, lowest.camera_ip)
    suggestion = None
    if fit is not None:
        try:
            suggestion = ettr.suggest(fit.target_signal, gain=gain,
                                      exposure_offset_us=lowest.exposure_offset_us, limit_ms=limit_ms,
                                      fit=fit)
        except ValueError as error:
            fit_error = str(error)
    return ExposureSeries(
        ok=suggestion is not None, reason=fit_error, chart_key=pending.chart_key, directory=str(directory),
        center_ms=pending.center_ms, gain=gain, camera_ip=lowest.camera_ip, device_id=lowest.device_id,
        exposure_offset_us=lowest.exposure_offset_us, patch=patch, points=points, fit=fit,
        fit_error=fit_error, limit_ms=limit_ms, limit_notes=tuple(limit_notes),
        initial_suggestion=suggestion, elapsed_s=time.monotonic() - pending.started,
        raw_output=pending.raw_output, selection=selection)


def _exposure_limit(camera_ip: str) -> tuple[float | None, list[str]]:
    if not camera_ip:
        return None, ["camera address unknown; the frame-period limit is checked on Apply"]
    return exposure_series.exposure_limit(lambda: config_store.gox_exposure_limit_ms(camera_ip))


async def _save(series: ExposureSeries, extra: dict | None = None) -> None:
    status = "completed" if series.ok else "awaiting_chart_selection" if series.awaiting_selection else "failed"
    await exposure_series.save(series.directory, {**series.to_dict(), "status": status, **(extra or {})})


# --- verification of a normal reference capture ----------------------------------

def _applicable(series: ExposureSeries | None, chart_key: str, device_id: str | None) -> bool:
    return (series is not None and series.ok and series.fit is not None and series.patch is not None
            and series.chart_key == chart_key and bool(device_id) and device_id == series.device_id)


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
    check = exposure_series.check(series, point, gain=point.gain, exposure_offset_us=series.exposure_offset_us,
                                  what="peak", again="capture")
    _series = dataclasses.replace(series, checks=series.checks + (check,))
    return check


def check_failed_capture(chart_key: str, measurement: color_checker.ChartMeasurement,
                         source: str) -> ExposureCheck | None:
    """A reference whose calibration failed (e.g. saturated): its measured saturation still counts."""
    series = _series
    if series is None or not series.ok:
        return None
    try:
        frame_exposure = frame_exposure_from_device(raw_recording.device_metadata(measurement.directory))
    except (OSError, ValueError, KeyError, TypeError):
        return None
    provenance = {"reference_exposure": frame_exposure.to_dict(), "source_full_scale_dn": measurement.full_scale}
    return check_reference(chart_key, provenance, source, measurement.status, measurement.means)


async def save_current() -> None:
    if _series is not None:
        await _save(_series)
