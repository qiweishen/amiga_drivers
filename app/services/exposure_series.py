"""Pieces shared by the GOX and FX10 ETTR exposure series (gox_exposure, fx10_exposure).

Each series captures a bracket of exposures under ``<Output Directory>/tmp``,
fits the brightest patch / band against exposure and suggests the exposure that
reaches the ETTR target. A later reference capture is then checked against that
target. Only the exposure is varied or suggested; nothing here edits a config.
"""

from __future__ import annotations

import asyncio
import uuid
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from ..decoder import exposure
from . import atomic_json, config_store, reference_storage


@dataclass(frozen=True)
class ExposureCheck:
    """A reference capture compared with the ETTR target of the active series."""

    point: exposure.ExposurePoint
    target_dn: float
    deviation: float | None  # None when the measured value saturated (only a lower bound is known)
    within: bool
    suggestion: exposure.Suggestion | None  # refined suggestion when outside the tolerance
    message: str

    def to_dict(self) -> dict:
        return {"point": self.point.to_dict(), "target_dn": self.target_dn, "deviation": self.deviation,
                "within_tolerance": self.within, "tolerance": exposure.TOLERANCE,
                "suggestion": self.suggestion.to_dict() if self.suggestion else None, "message": self.message}


class Suggested:
    """The current suggestion of a series with ``checks`` and an ``initial_suggestion``."""

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


def prepare(prefix: str, config_name: str, raw_config: bytes, document: dict) -> tuple[Path, Path]:
    """A new series directory with a copy of the snapshot config and a first series.json."""
    now = datetime.now(timezone.utc)
    directory = reference_storage.tmp_root() / f"{prefix}_{now:%Y%m%dT%H%M%SZ}_{uuid.uuid4().hex[:8]}"
    config = directory / "config" / config_name
    directory.mkdir(parents=True, exist_ok=False)
    config.parent.mkdir()
    config.write_bytes(raw_config)
    atomic_json.write(directory / "series.json", {
        **document, "status": "capturing", "created_utc": now.isoformat(),
        "config_snapshot": config.relative_to(directory).as_posix()})
    return directory, config


async def save(directory: str, document: dict) -> None:
    if directory:
        try:
            await asyncio.to_thread(atomic_json.write, Path(directory) / "series.json", document)
        except (OSError, ValueError):
            pass  # the in-memory result still reports the outcome


def exposure_limit(read_limit: Callable[[], tuple[float | None, list[str]]]) -> tuple[float | None, list[str]]:
    """The exposure ceiling of the configs an Apply writes; checked on Apply when unavailable."""
    try:
        return read_limit()
    except (OSError, ValueError, config_store.ApplyError) as error:
        return None, [f"frame-period limit unavailable ({error}); it is checked on Apply"]


def _refit(series, extra: exposure.ExposurePoint, *, exposure_offset_us: float, unchanged: str | None,
           again: str) -> tuple[exposure.Suggestion | None, str]:
    points = [point.point for point in series.points if point.point is not None]
    points += [check.point for check in series.checks] + [extra]
    try:
        suggestion, prefix = exposure.refine(series.fit, points, extra, exposure_offset_us=exposure_offset_us,
                                             limit_ms=series.limit_ms, unchanged=unchanged)
    except ValueError as error:
        return None, f"Refit failed: {error}"
    note = f" {suggestion.note}" if suggestion.note else ""
    return suggestion, f"{prefix} {suggestion.exposure_ms:g} ms; apply it and {again} again.{note}"


def check(series, point: exposure.ExposurePoint, *, gain: float, exposure_offset_us: float, what: str,
          again: str, unchanged: str | None = None) -> ExposureCheck:
    """Compare ``point`` with the series' target; refit with it when it is outside the tolerance.

    ``what`` names the measured value ("peak", "p90"); ``again`` the operator's next step.
    """
    target, ceiling_ms = exposure.reachable_target(series.fit, gain=gain, exposure_offset_us=exposure_offset_us,
                                                   limit_ms=series.limit_ms)
    reach = "" if ceiling_ms is None else (
        f" The {series.limit_ms:g} ms frame-period limit caps the exposure at {ceiling_ms:g} ms, so the "
        f"reachable {what} ({target:.0f} DN) is the target instead of {series.fit.target_dn:.0f} DN.")
    deviation = exposure.deviation(point.peak_dn, target) if point.usable else None
    if deviation is not None and abs(deviation) <= exposure.TOLERANCE:
        return ExposureCheck(point, target, deviation, True, None,
                             f"Within ±{exposure.TOLERANCE:.0%} of the ETTR target.{reach}")
    suggestion, message = _refit(series, point, exposure_offset_us=exposure_offset_us, unchanged=unchanged,
                                 again=again)
    return ExposureCheck(point, target, deviation, False, suggestion, message)
