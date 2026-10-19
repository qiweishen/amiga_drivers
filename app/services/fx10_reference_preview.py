"""Whole-phase reference presentation; recording interpretation lives in app.formats.fx10."""

from __future__ import annotations

import csv
from dataclasses import dataclass, field
from pathlib import Path

from ..formats import fx10


@dataclass(frozen=True)
class ReferenceSpectrum:
    ok: bool
    reason: str = ""
    wavelengths_nm: list[float] = field(default_factory=list)
    spectrum_dn: dict[str, list[float]] = field(default_factory=dict)
    full_scale: int = 0
    pixel_format: str = ""
    frames: int = 0
    bands: int = 0
    samples: int = 0
    mean_dn: float = 0.0
    clipped_pct: float = 0.0


def read_spectrum(session_dir: Path, expected_frames: int) -> ReferenceSpectrum:
    """Decode all finalized segments; a preview failure never changes raw data."""
    try:
        capture, count, values, mean, clipped = fx10.reference_spectrum(
            session_dir, expected_frames, normalized=False)
        return ReferenceSpectrum(True, wavelengths_nm=list(capture.wavelengths_nm), spectrum_dn=values,
                                 full_scale=capture.full_scale, pixel_format=capture.pixel_format,
                                 frames=count, bands=capture.bands, samples=capture.samples,
                                 mean_dn=mean, clipped_pct=clipped)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError, csv.Error) as error:
        return ReferenceSpectrum(False, reason=f"Cannot display reference spectrum: {error}")
