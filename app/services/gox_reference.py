"""GOX colour-reference capture, calibration scheduling and reference metadata."""

from __future__ import annotations

import asyncio
import dataclasses
import json
import os
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from ..constants import SNAPSHOT_CONFIG
from ..decoder.gox import color_checker
from . import (atomic_json, camera_operations, config_store, gox_chart, gox_exposure, gox_tools, preview_images,
               reference_storage)

_status = ""
_last_result: ReferenceResult | None = None
_pending: _Pending | None = None


@dataclass
class _Pending:
    """A captured reference waiting for the operator to select the chart."""

    session: Path
    config: Path
    document: dict
    camera_dir: Path
    chart_key: str
    detection: color_checker.Detection
    raw_output: str
    started: float


@dataclass(frozen=True)
class ReferenceResult:
    ok: bool
    reason: str = ""
    chart_key: str = ""
    session_dir: str = ""
    elapsed_s: float = 0.0
    raw_output: str = ""
    analysis: color_checker.CalibrationPreview | None = None
    config_snapshot: str = ""
    # URL paths from preview_images: "<kind>" is the 8-bit screen image and
    # "<kind>_full" the on-demand full-resolution 16-bit PNG, for each kind in
    # color_checker.FULL_RESOLUTION_KINDS.
    images: dict[str, str] = field(default_factory=dict)
    # gox_exposure.ExposureCheck against the active ETTR series, when applicable.
    exposure_check: object | None = None
    saved: bool = False  # moved from the output directory's tmp folder into the output directory
    # Detector candidates waiting for the operator (choose / reject); empty otherwise.
    candidates: tuple[gox_chart.CandidateView, ...] = ()

    @property
    def awaiting_selection(self) -> bool:
        return bool(self.candidates)


def last_result() -> ReferenceResult | None:
    return _last_result


def status() -> str:
    return _status


def _image_stem(analysis: color_checker.CalibrationPreview) -> str:
    source = analysis.source_dir
    session = source.parents[2].name if source is not None and len(source.parents) > 2 else "gox_reference"
    return f"{session}_{analysis.chart_key}"


def _register_images(analysis: color_checker.CalibrationPreview) -> dict[str, str]:
    stem = _image_stem(analysis)
    images = {}
    for kind, data in (("original", analysis.original_png), ("annotated", analysis.annotated_png),
                       ("corrected", analysis.corrected_png)):
        images[kind] = preview_images.put(data, f"{stem}_{kind}_screen.png")
        images[f"{kind}_full"] = preview_images.lazy(
            lambda kind=kind: color_checker.full_resolution_png(analysis, kind),
            f"{stem}_{kind}_16bit.png")
    return images


def _release(result: ReferenceResult | None) -> None:
    if result is not None:
        preview_images.release(*result.images.values())
        gox_chart.release(result.candidates)


def _prepare(chart_key: str):
    root = reference_storage.tmp_root()
    raw = config_store.snapshot_config_bytes(SNAPSHOT_CONFIG)
    now = datetime.now(timezone.utc)
    session = root / f"reference_gox_{now:%Y%m%dT%H%M%SZ}_{uuid.uuid4().hex[:8]}"
    config = session / "raw" / "config" / "config-gox-snapshot.yaml"
    document = {
        "format": "gox-color-reference-v1", "status": "capturing", "created_utc": now.isoformat(),
        "chart_type": chart_key, "target_color_space": "sRGB", "target_white_point": "D65",
        "config_source": str(SNAPSHOT_CONFIG),
        "config_snapshot": config.relative_to(session).as_posix(),
        "capture_mode": "single-frame snapshot; freerun; PTP, hardware trigger and SensorSync disabled",
        "scope": "reference calibration and GUI preview; original RAW is retained unchanged",
    }
    session.mkdir(parents=True, exist_ok=False)
    return session, config, document, raw


def _initialize(session: Path, config: Path, document: dict, raw: bytes) -> None:
    # The caller already knows the newly created session path, including when
    # saving these initial files fails partway through.
    config.parent.mkdir(parents=True)
    with config.open("xb") as saved:
        saved.write(raw)
        saved.flush()
        os.fsync(saved.fileno())
    atomic_json.write(session / "reference.json", document)


async def capture(chart_key: str) -> ReferenceResult:
    """Capture with snapshot-config parameters after an explicit chart selection."""
    gox_chart.check_key(chart_key)
    if reason := color_checker.capability_reason():
        raise ValueError(reason)
    return await camera_operations.run(
        "gox", lambda: _capture(chart_key), kind="gox-color-reference")


async def _write_failed(session: Path | None, document: dict | None, failure: str, check=None) -> str:
    """Record a failed reference; returns the failure, extended if the record cannot be written."""
    if session is None or document is None:
        return failure
    if check is not None:
        document["exposure_check"] = check.to_dict()
    document.update(status="failed", error=failure, ended_utc=datetime.now(timezone.utc).isoformat())
    document.pop("calibration", None)
    document.pop("completed_utc", None)
    try:
        await asyncio.to_thread(atomic_json.write, session / "reference.json", document)
    except (OSError, ValueError) as save_error:
        failure += f"; failure metadata could not be saved: {save_error}"
    return failure


async def _abandon_pending(reason: str) -> None:
    """A capture still waiting for a chart selection is failed when a new one replaces it."""
    global _pending
    pending, _pending = _pending, None
    if pending is not None:
        await _write_failed(pending.session, pending.document, reason)


async def _capture(chart_key: str) -> ReferenceResult:
    """Capture and detect; the result waits for the operator's chart selection (choose / reject)."""
    global _status, _last_result, _pending
    started = time.monotonic()
    await _abandon_pending("Replaced by a new reference capture before a chart was selected")
    _release(_last_result)
    _last_result = None
    _status = f"Preparing {chart_key} reference capture…"
    session = None
    config = None
    document = None
    raw_output = ""
    detection = None
    failure = ""
    camera_dir = None
    try:
        session, config, document, raw_config = await asyncio.to_thread(_prepare, chart_key)
        await asyncio.to_thread(_initialize, session, config, document, raw_config)
        _status = f"Capturing GOX RAW for {chart_key}…"
        captured = await gox_tools.capture_raw(None, session / "raw" / "gox", config=config)
        raw_output = captured.raw_output
        if not captured.ok or captured.camera_dir is None:
            raise ValueError(captured.reason or "GOX capture did not return a camera recording")
        document["camera_directory"] = captured.camera_dir.relative_to(session).as_posix()
        document["device_metadata"] = (captured.camera_dir / "device.json").relative_to(session).as_posix()
        camera_dir = captured.camera_dir
        _status = f"Detecting {chart_key} candidates…"
        detection = await asyncio.to_thread(color_checker.detect, camera_dir, chart_key)
        document.update(status="awaiting_chart_selection", detection_transfer=detection.transfer,
                        chart_candidates=[candidate.to_dict() for candidate in detection.candidates])
        await asyncio.to_thread(atomic_json.write, session / "reference.json", document)
    except asyncio.CancelledError:
        _status = "GOX reference capture interrupted; any acquired RAW is retained"
        raise
    except Exception as error:
        failure = await _write_failed(session, document, str(error) or type(error).__name__)
    common = {"chart_key": chart_key, "session_dir": str(session) if session else "",
              "elapsed_s": time.monotonic() - started, "raw_output": raw_output,
              "config_snapshot": str(config) if config else ""}
    if failure:
        _last_result = ReferenceResult(ok=False, reason=failure, **common)
        _status = f"GOX reference failed: {failure}"
        return _last_result
    _pending = _Pending(session, config, document, camera_dir, chart_key, detection, raw_output, started)
    offered = [(f"Candidate {candidate.number}", candidate) for candidate in detection.candidates]
    _last_result = ReferenceResult(ok=False, candidates=gox_chart.register(session.name, offered), **common)
    _status = (f"{len(offered)} {chart_key} candidate(s) detected; select the chart "
               "(only you can tell the chart from a regular texture)")
    return _last_result


def _take_pending() -> _Pending:
    global _pending
    pending, _pending = _pending, None  # one decision per capture, whichever client makes it
    if pending is None:
        raise ValueError("No reference capture is waiting for a chart selection")
    return pending


async def choose(number: int) -> ReferenceResult:
    """Calibrate the waiting capture with the candidate the operator selected."""
    global _status, _last_result
    pending = _pending
    if pending is None:
        raise ValueError("No reference capture is waiting for a chart selection")
    candidate = next((item for item in pending.detection.candidates if item.number == number), None)
    if candidate is None:
        raise ValueError(f"Candidate {number} was not offered")
    pending = _take_pending()
    session, document, chart_key = pending.session, pending.document, pending.chart_key
    selection = {"candidate": candidate.number, "opencv_cost": candidate.cost,
                 "candidate_count": len(pending.detection.candidates), "selected_by": "operator"}
    _status = f"Fitting candidate {candidate.number} of the {chart_key} reference to sRGB (D65)…"
    measurement = analysis = check = None
    failure = ""
    try:
        measurement = await asyncio.to_thread(
            color_checker.measure, pending.camera_dir, chart_key, candidate.polygons,
            geometry=color_checker.GEOMETRY_SELECTED, detection_transfer=pending.detection.transfer,
            selection=selection)
        analysis = await asyncio.to_thread(color_checker.calibrate, pending.camera_dir, chart_key,
                                           measurement=measurement)
        check = gox_exposure.check_reference(chart_key, analysis.provenance, session.name)
        if check is not None:
            document["exposure_check"] = check.to_dict()
            await gox_exposure.save_current()
        document.update(
            status="completed", completed_utc=datetime.now(timezone.utc).isoformat(), chart_selection=selection,
            calibration={"matrix": analysis.matrix, "loss": analysis.loss,
                         "patch_count": analysis.patch_count, "used_patches": analysis.used_patches,
                         "pixel_format": analysis.pixel_format, "full_scale": analysis.full_scale,
                         "provenance": analysis.provenance,
                         # Reusable ColorCalibration (scale_by_exposure_ratio), or null with
                         # provenance.reference_exposure_unavailable_reason.
                         "color_calibration": analysis.color_calibration},
        )
        await asyncio.to_thread(atomic_json.write, session / "reference.json", document)
    except asyncio.CancelledError:
        _status = "GOX reference calibration interrupted; the RAW is retained"
        raise
    except Exception as error:
        failure = str(error) or type(error).__name__
        analysis = None
        if measurement is not None:
            # e.g. an over-exposed verification capture: report its saturation to the series.
            check = gox_exposure.check_failed_capture(chart_key, measurement, session.name)
            if check is not None:
                await gox_exposure.save_current()
        document["chart_selection"] = selection
        failure = await _write_failed(session, document, failure, check)
    del measurement  # full-resolution native RGB
    _release(_last_result)
    _last_result = ReferenceResult(
        ok=not failure, reason=failure, chart_key=chart_key, session_dir=str(session),
        elapsed_s=time.monotonic() - pending.started, raw_output=pending.raw_output, analysis=analysis,
        config_snapshot=str(pending.config), images=_register_images(analysis) if analysis is not None else {},
        exposure_check=check,
    )
    _status = (f"GOX reference captured in {session} (not yet saved to the output directory)" if not failure
               else f"GOX reference failed: {failure}")
    return _last_result


async def reject() -> ReferenceResult:
    """None of the candidates is the chart: the reference fails and its RAW is kept."""
    global _status, _last_result
    pending = _take_pending()
    failure = await _write_failed(pending.session, pending.document, gox_chart.NO_CHART_SELECTED)
    _release(_last_result)
    _last_result = ReferenceResult(
        ok=False, reason=failure, chart_key=pending.chart_key, session_dir=str(pending.session),
        elapsed_s=time.monotonic() - pending.started, raw_output=pending.raw_output,
        config_snapshot=str(pending.config))
    _status = f"GOX reference failed: {failure}"
    return _last_result


def _record_save(session: Path, note: dict) -> None:
    document = json.loads((session / "reference.json").read_text(encoding="utf-8"))
    atomic_json.write(session / "reference.json", {**document, **note})


async def save_to_data() -> ReferenceResult:
    """Move the last completed reference from <output>/tmp into <output>."""
    global _last_result, _status
    result = _last_result
    if result is None or not result.ok or result.analysis is None or not result.session_dir:
        raise ValueError("There is no completed reference to save")
    if result.saved:
        raise ValueError(f"This reference is already saved: {result.session_dir}")
    session = Path(result.session_dir)
    target = await asyncio.to_thread(reference_storage.save, session, _record_save)
    analysis = result.analysis
    if analysis.source_dir is not None:
        analysis = dataclasses.replace(analysis, source_dir=target / analysis.source_dir.relative_to(session))
    config = Path(result.config_snapshot) if result.config_snapshot else None
    _release(result)
    _last_result = dataclasses.replace(
        result, session_dir=str(target), analysis=analysis, saved=True,
        config_snapshot=str(target / config.relative_to(session)) if config else "",
        images=_register_images(analysis))
    _status = f"GOX reference saved: {target}"
    return _last_result
