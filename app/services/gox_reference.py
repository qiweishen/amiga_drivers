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
from . import atomic_json, camera_operations, config_store, gox_tools, preview_images, reference_storage

_status = ""
_last_result: ReferenceResult | None = None
MAX_CONFIG_BYTES = 1024 * 1024
MAX_REFERENCE_BYTES = 4 * 1024 * 1024


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


def capability_reason() -> str | None:
    return color_checker.capability_reason()


def guard_reason() -> str | None:
    return camera_operations.guard_reason("gox")


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


def _validate_chart(chart_key: str) -> None:
    if not isinstance(chart_key, str) or chart_key not in color_checker.CHARTS:
        raise ValueError("Explicitly select MCC24, SG140 or VINYL18 before capturing a reference")


def _config_bytes() -> bytes:
    def signature(stat):
        return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns

    path = config_store.checked_snapshot_path(SNAPSHOT_CONFIG)
    with path.open("rb") as source:
        before = signature(os.fstat(source.fileno()))
        raw = source.read(MAX_CONFIG_BYTES + 1)
        if before != signature(os.fstat(source.fileno())) or before != signature(path.stat()):
            raise ValueError("GOX snapshot configuration changed while being read; retry")
    if len(raw) > MAX_CONFIG_BYTES:
        raise ValueError("GOX snapshot configuration exceeds the reference capture limit")
    raw.decode("utf-8")
    return raw


TMP_FOLDER = reference_storage.TMP_FOLDER


def _output_root() -> Path:
    return reference_storage.output_root()


def reference_tmp_root() -> Path:
    """Unconfirmed references and exposure series live in <output>/tmp until saved."""
    return _output_root() / TMP_FOLDER


def _prepare(chart_key: str):
    root = reference_tmp_root()
    raw = _config_bytes()
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
    atomic_json.write(session / "reference.json", document, limit=MAX_REFERENCE_BYTES)


async def capture(chart_key: str) -> ReferenceResult:
    """Capture with snapshot-config parameters after an explicit chart selection."""
    _validate_chart(chart_key)
    if reason := capability_reason():
        raise ValueError(reason)
    return await camera_operations.run(
        "gox", lambda: _capture(chart_key), kind="gox-color-reference")


async def _capture(chart_key: str) -> ReferenceResult:
    global _status, _last_result
    started = time.monotonic()
    _release(_last_result)
    _last_result = None
    _status = f"Preparing {chart_key} reference capture…"
    session = None
    config = None
    document = None
    raw_output = ""
    analysis = None
    failure = ""
    check = None
    camera_dir = None
    from . import gox_exposure  # imports this module; resolved at call time
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
        _status = f"Detecting {chart_key} and fitting native camera RGB to sRGB (D65)…"
        analysis = await asyncio.to_thread(color_checker.calibrate, captured.camera_dir, chart_key)
        check = gox_exposure.check_reference(chart_key, analysis.provenance, session.name)
        if check is not None:
            document["exposure_check"] = check.to_dict()
            await gox_exposure.save_current()
        document.update(
            status="completed", completed_utc=datetime.now(timezone.utc).isoformat(),
            calibration={"matrix": analysis.matrix, "loss": analysis.loss,
                         "patch_count": analysis.patch_count, "used_patches": analysis.used_patches,
                         "pixel_format": analysis.pixel_format, "full_scale": analysis.full_scale,
                         "provenance": analysis.provenance,
                         # Reusable ColorCalibration (scale_by_exposure_ratio), or null with
                         # provenance.reference_exposure_unavailable_reason.
                         "color_calibration": analysis.color_calibration},
        )
        await asyncio.to_thread(atomic_json.write, session / "reference.json", document, limit=MAX_REFERENCE_BYTES)
    except asyncio.CancelledError:
        _status = "GOX reference capture interrupted; any acquired RAW is retained"
        # A manifest is successful only after the completed calibration was written.
        raise
    except Exception as error:
        failure = str(error) or type(error).__name__
        if camera_dir is not None and analysis is None:
            # e.g. an over-exposed verification capture: report its saturation to the series.
            check = await asyncio.to_thread(gox_exposure.check_failed_capture, chart_key, camera_dir,
                                            session.name)
            if check is not None:
                await gox_exposure.save_current()
        if session is not None and document is not None:
            if check is not None:
                document["exposure_check"] = check.to_dict()
            document.update(status="failed", error=failure, ended_utc=datetime.now(timezone.utc).isoformat())
            document.pop("calibration", None)
            document.pop("completed_utc", None)
            try:
                await asyncio.to_thread(atomic_json.write, session / "reference.json", document,
                                        limit=MAX_REFERENCE_BYTES)
            except (OSError, ValueError) as save_error:
                failure += f"; failure metadata could not be saved: {save_error}"
    _last_result = ReferenceResult(
        ok=not failure, reason=failure, chart_key=chart_key, session_dir=str(session) if session else "",
        elapsed_s=time.monotonic() - started, raw_output=raw_output,
        analysis=analysis if not failure else None, config_snapshot=str(config) if config else "",
        images=_register_images(analysis) if analysis is not None and not failure else {},
        exposure_check=check,
    )
    _status = (f"GOX reference captured in {session} (not yet saved to the output directory)" if not failure
               else f"GOX reference failed: {failure}")
    return _last_result


def _move_to_output(session: Path) -> Path:
    root = _output_root()
    if session.parent != root / TMP_FOLDER:
        raise ValueError(f"Only references under {root / TMP_FOLDER} can be saved")
    if (root / session.name).exists():
        raise ValueError(f"{root / session.name} already exists")
    document = json.loads((session / "reference.json").read_text(encoding="utf-8"))
    document["saved_utc"] = datetime.now(timezone.utc).isoformat()
    document["saved_from"] = session.relative_to(root).as_posix()
    atomic_json.write(session / "reference.json", document, limit=MAX_REFERENCE_BYTES)
    return reference_storage.move_to_output(session, root)


async def save_to_data() -> ReferenceResult:
    """Move the last completed reference from <output>/tmp into <output>."""
    global _last_result, _status
    result = _last_result
    if result is None or not result.ok or result.analysis is None or not result.session_dir:
        raise ValueError("There is no completed reference to save")
    if result.saved:
        raise ValueError(f"This reference is already saved: {result.session_dir}")
    session = Path(result.session_dir)
    target = await asyncio.to_thread(_move_to_output, session)
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
