"""FX10 ETTR exposure series on the white reference: bracket snapshots, parallel analysis, exposure fit.

The camera is exclusive, so bracket exposures (center ± 15 ms) are captured one
after another with fx10_snapshot; each capture's spectrum is decoded in a worker
thread while the next exposure is captured. The brightest band's p90 DN is
fitted against the ExposureTime readback and the exposure that puts it at the
chosen fraction of full scale (default 60 %) is suggested.

Only the exposure is varied or suggested. Binning (and every other setting)
comes from the snapshot configuration and is never overridden or recommended.
Series sessions stay under the output directory's ``tmp`` folder; nothing here
edits a configuration.
"""

from __future__ import annotations

import asyncio
import dataclasses
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from ..constants import FX10_SNAPSHOT_CONFIG
from ..decoder import fx10
from ..decoder.ebus import recording as raw_recording
from ..decoder.fx10 import exposure as fx10_exposure
from ..decoder.gox import exposure
from . import atomic_json, camera_operations, config_store, fx10_tools, reference_storage

MAX_SERIES_BYTES = 4 * 1024 * 1024
MAX_CONFIG_BYTES = 1024 * 1024
EXPOSURE_OFFSET_US = 0.0  # no documented FX10 integration offset; the fitted intercept absorbs any

_status = ""
_series: ExposureSeries | None = None


@dataclass(frozen=True)
class BracketPoint:
    requested_exposure_ms: float
    session: str  # snapshot session relative to the series directory
    point: exposure.ExposurePoint | None
    mean_dn: float | None
    clipped_pct: float | None
    error: str = ""

    def to_dict(self) -> dict:
        return {"requested_exposure_ms": self.requested_exposure_ms, "session": self.session,
                "point": self.point.to_dict() if self.point else None, "cube_mean_dn": self.mean_dn,
                "cube_clipped_pct": self.clipped_pct, "error": self.error or None}


@dataclass(frozen=True)
class ExposureCheck:
    """A white reference compared with the ETTR target of the active series."""

    point: exposure.ExposurePoint
    target_dn: float
    deviation: float | None  # None when the band saturated (only a lower bound is known)
    within: bool
    suggestion: exposure.Suggestion | None
    message: str

    def to_dict(self) -> dict:
        return {"point": self.point.to_dict(), "target_dn": self.target_dn, "deviation": self.deviation,
                "within_tolerance": self.within, "tolerance": exposure.TOLERANCE,
                "suggestion": self.suggestion.to_dict() if self.suggestion else None, "message": self.message}


@dataclass(frozen=True)
class ExposureSeries:
    ok: bool
    reason: str = ""
    directory: str = ""
    center_ms: float = 0.0
    target_fraction: float = fx10_exposure.DEFAULT_TARGET_FRACTION
    device_id: str = ""
    binning: tuple[int, int] | None = None  # (spatial, spectral) readback, unchanged config values
    bands: int = 0
    full_scale: int = 0
    band: int | None = None  # 1-based band whose p90 is fitted
    band_label: str = ""
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
            "format": "fx10-exposure-series-v1", "ok": self.ok, "reason": self.reason or None,
            "center_exposure_ms": self.center_ms, "bracket_step_ms": fx10_exposure.BRACKET_STEP_MS,
            "target_fraction": self.target_fraction, "tolerance": exposure.TOLERANCE,
            "statistic": f"per-band {fx10_exposure.STATISTIC} DN over every frame and spatial sample",
            "device_id": self.device_id, "binning": list(self.binning) if self.binning else None,
            "bands": self.bands, "full_scale": self.full_scale, "band": self.band, "band_label": self.band_label,
            "exposure_offset_us": EXPOSURE_OFFSET_US,
            "points": [point.to_dict() for point in self.points],
            "checks": [check.to_dict() for check in self.checks],
            "fit": self.fit.to_dict() if self.fit else None, "fit_error": self.fit_error or None,
            "exposure_limit_ms": self.limit_ms, "exposure_limit_sources": list(self.limit_notes),
            "initial_suggestion": self.initial_suggestion.to_dict() if self.initial_suggestion else None,
            "suggestion": self.suggestion.to_dict() if self.suggestion else None,
            "scope": "exposure estimation only; configs are changed only by an explicit Apply (exposure only)",
        }


def last_series() -> ExposureSeries | None:
    return _series


def status() -> str:
    return _status


@dataclass
class _Analysis:
    session_dir: Path
    requested_exposure_ms: float
    p90: list[float] | None = None
    wavelengths_nm: tuple[float, ...] = ()
    full_scale: int = 0
    bands: int = 0
    mean_dn: float | None = None
    clipped_pct: float | None = None
    readback: dict | None = None
    error: str = ""


def _analyze(session_dir: Path, requested_ms: float) -> _Analysis:
    """Decode one bracket snapshot into per-band statistics. Runs in a worker thread."""
    result = _Analysis(session_dir, requested_ms)
    try:
        result.readback = fx10_exposure.exposure_from_device(raw_recording.device_metadata(session_dir))
        capture, cube = fx10.snapshot_cube(session_dir)
        values, result.mean_dn, result.clipped_pct = fx10.spectrum(cube, capture.full_scale)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError) as error:
        result.error = str(error) or type(error).__name__
        return result
    result.p90, result.wavelengths_nm = values[fx10_exposure.STATISTIC], capture.wavelengths_nm
    result.full_scale, result.bands = capture.full_scale, capture.bands
    return result


def _config_bytes() -> bytes:
    path = config_store.checked_snapshot_path(FX10_SNAPSHOT_CONFIG)
    with path.open("rb") as source:
        raw = source.read(MAX_CONFIG_BYTES + 1)
    if len(raw) > MAX_CONFIG_BYTES:
        raise ValueError("FX10 snapshot configuration exceeds the capture limit")
    return raw


def _prepare_series(exposures: list[float], target_fraction: float):
    root = reference_storage.tmp_root()
    raw = _config_bytes()
    now = datetime.now(timezone.utc)
    directory = root / f"exposure_fx10_{now:%Y%m%dT%H%M%SZ}_{uuid.uuid4().hex[:8]}"
    config = directory / "config" / "config-fx10-snapshot.yaml"
    directory.mkdir(parents=True, exist_ok=False)
    config.parent.mkdir()
    config.write_bytes(raw)
    atomic_json.write(directory / "series.json", {
        "format": "fx10-exposure-series-v1", "status": "capturing", "created_utc": now.isoformat(),
        "exposures_ms": exposures, "target_fraction": target_fraction,
        "binning": "unchanged (snapshot config)", "config_source": str(FX10_SNAPSHOT_CONFIG),
        "config_snapshot": config.relative_to(directory).as_posix(),
    }, limit=MAX_SERIES_BYTES)
    return directory, config


async def run(center_ms: float, target_fraction: float = fx10_exposure.DEFAULT_TARGET_FRACTION) -> ExposureSeries:
    """Bracket the exposure only; binning stays whatever the snapshot config sets."""
    exposures = fx10_exposure.bracket(center_ms)
    target_fraction = fx10_exposure.target_fraction(target_fraction)
    return await camera_operations.run(
        "fx10", lambda: _run(float(center_ms), exposures, target_fraction), kind="fx10-exposure-series")


def _point(analysis: _Analysis, band: int, directory: Path) -> exposure.ExposurePoint:
    return fx10_exposure.point_from_spectrum(
        f"bracket {analysis.requested_exposure_ms:g} ms", analysis.p90, band,
        analysis.readback["exposure_time_us"], analysis.full_scale, analysis.wavelengths_nm,
        requested_exposure_ms=analysis.requested_exposure_ms,
        source=analysis.session_dir.relative_to(directory).as_posix())


def _signature(analysis: _Analysis) -> tuple:
    readback = analysis.readback
    return (readback["device_id"], readback["spatial_binning"], readback["spectral_binning"],
            analysis.bands, analysis.full_scale)


async def _run(center_ms: float, exposures: list[float], target_fraction: float) -> ExposureSeries:
    global _status, _series
    started = time.monotonic()
    _series = None
    directory = None
    raw_outputs = []
    try:
        _status = "Preparing the FX10 exposure series…"
        directory, config = await asyncio.to_thread(_prepare_series, exposures, target_fraction)
        pending: list[asyncio.Task | _Analysis] = []
        for index, exposure_ms in enumerate(exposures, 1):
            _status = f"Capturing exposure {index}/{len(exposures)}: {exposure_ms:g} ms…"
            output = directory / f"exposure_{index}_{exposure_ms:g}ms"
            captured = await fx10_tools.capture_raw(output, exposure_ms, config=config)
            raw_outputs.append(f"--- {exposure_ms:g} ms ---\n{captured.raw_output}")
            if not captured.ok or not captured.session_dir:
                pending.append(_Analysis(output, exposure_ms, error=captured.reason or "capture failed"))
                continue
            # Decoding overlaps the next capture; the camera itself is never shared.
            pending.append(asyncio.create_task(
                asyncio.to_thread(_analyze, Path(captured.session_dir), exposure_ms)))
        _status = "Analyzing the FX10 exposure series…"
        analyses = [item if isinstance(item, _Analysis) else await item for item in pending]
        measured = [item for item in analyses if item.p90 is not None]
        if not measured:
            reasons = "; ".join(f"{item.requested_exposure_ms:g} ms: {item.error}" for item in analyses)
            raise ValueError(f"No exposure produced a usable spectrum ({reasons})")
        signatures = {_signature(item) for item in measured}
        if len(signatures) != 1:
            raise ValueError("The bracket captures disagree on camera, binning or geometry; "
                             "the series varies the exposure only")
        device_id, spatial, spectral, bands, full_scale = signatures.pop()
        lowest = min(measured, key=lambda item: item.readback["exposure_time_us"])
        band = fx10_exposure.brightest_band(lowest.p90)
        label = fx10_exposure.band_label(band, lowest.wavelengths_nm)
        points = tuple(BracketPoint(
            item.requested_exposure_ms,
            item.session_dir.relative_to(directory).as_posix() if item.session_dir.is_relative_to(directory)
            else str(item.session_dir),
            _point(item, band, directory) if item.p90 is not None else None,
            item.mean_dn, item.clipped_pct, item.error) for item in analyses)
        fit, fit_error = None, ""
        try:
            fit = exposure.fit([point.point for point in points if point.point is not None], gain=1.0,
                               exposure_offset_us=EXPOSURE_OFFSET_US, full_scale=full_scale,
                               target_fraction=target_fraction)
        except ValueError as error:
            fit_error = str(error)
        limit_ms, limit_notes = await asyncio.to_thread(_exposure_limit)
        suggestion = None
        if fit is not None:
            try:
                suggestion = exposure.suggest(fit.target_signal, gain=1.0, exposure_offset_us=EXPOSURE_OFFSET_US,
                                              limit_ms=limit_ms, fit=fit, unchanged=fx10_exposure.UNCHANGED)
            except ValueError as error:
                fit_error = str(error)
        series = ExposureSeries(
            ok=suggestion is not None, reason=fit_error, directory=str(directory), center_ms=center_ms,
            target_fraction=target_fraction, device_id=device_id, binning=(spatial, spectral), bands=bands,
            full_scale=full_scale, band=band, band_label=label, points=points, fit=fit, fit_error=fit_error,
            limit_ms=limit_ms, limit_notes=tuple(limit_notes), initial_suggestion=suggestion,
            elapsed_s=time.monotonic() - started, raw_output="\n".join(raw_outputs))
    except asyncio.CancelledError:
        _status = "Exposure series interrupted; captured RAW is retained"
        raise
    except Exception as error:
        series = ExposureSeries(ok=False, reason=str(error) or type(error).__name__,
                                directory=str(directory) if directory else "", center_ms=center_ms,
                                target_fraction=target_fraction, elapsed_s=time.monotonic() - started,
                                raw_output="\n".join(raw_outputs))
    await _save(series)
    _series = series
    _status = (f"Suggested exposure {series.suggestion.exposure_ms:g} ms (ETTR {target_fraction:.0%} FS)"
               if series.ok else f"Exposure series failed: {series.reason}")
    return series


def _exposure_limit() -> tuple[float | None, list[str]]:
    try:
        return config_store.fx10_exposure_limit_ms()
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


# --- verification of a white reference --------------------------------------------

def _refit(series: ExposureSeries, extra: exposure.ExposurePoint) -> tuple[exposure.Suggestion | None, str]:
    points = [point.point for point in series.points if point.point is not None]
    points += [check.point for check in series.checks] + [extra]
    try:
        suggestion, prefix = exposure.refine(series.fit, points, extra, exposure_offset_us=EXPOSURE_OFFSET_US,
                                             limit_ms=series.limit_ms, unchanged=fx10_exposure.UNCHANGED)
    except ValueError as error:
        return None, f"Refit failed: {error}"
    note = f" {suggestion.note}" if suggestion.note else ""
    return suggestion, f"{prefix} {suggestion.exposure_ms:g} ms; apply it and collect again.{note}"


def check_reference(white_dir: Path, p90: list[float], full_scale: int, source: str) -> ExposureCheck | None:
    """Compare a white reference with the active series' ETTR target; None if not applicable.

    Applicable only for the same camera, binning, band count and full scale as
    the series; ``p90`` is the white phase's per-band p90 over all frames.
    """
    global _series
    series = _series
    if series is None or not series.ok or series.fit is None or series.band is None:
        return None
    try:
        readback = fx10_exposure.exposure_from_device(raw_recording.device_metadata(white_dir))
    except (OSError, ValueError, KeyError, TypeError):
        return None
    if ((readback["device_id"], (readback["spatial_binning"], readback["spectral_binning"]), len(p90), full_scale)
            != (series.device_id, series.binning, series.bands, series.full_scale)):
        return None
    point = fx10_exposure.point_from_spectrum("white reference", p90, series.band, readback["exposure_time_us"],
                                              full_scale, source=source)
    target, ceiling_ms = exposure.reachable_target(series.fit, gain=1.0, exposure_offset_us=EXPOSURE_OFFSET_US,
                                                   limit_ms=series.limit_ms)
    reach = "" if ceiling_ms is None else (
        f" The {series.limit_ms:g} ms frame-period limit caps the exposure at {ceiling_ms:g} ms, so the reachable "
        f"p90 ({target:.0f} DN) is the target instead of {series.fit.target_dn:.0f} DN.")
    if not point.usable:
        suggested, message = _refit(series, point)
        check = ExposureCheck(point, target, None, False, suggested, message)
    else:
        deviation = exposure.deviation(point.peak_dn, target)
        within = abs(deviation) <= exposure.TOLERANCE
        suggested, message = (None, f"Within ±{exposure.TOLERANCE:.0%} of the ETTR target.{reach}") \
            if within else _refit(series, point)
        check = ExposureCheck(point, target, deviation, within, suggested, message)
    _series = dataclasses.replace(series, checks=series.checks + (check,))
    return check


async def save_current() -> None:
    if _series is not None:
        await _save(_series)
