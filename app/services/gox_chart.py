"""Chart selection shared by the GOX colour reference and the GOX exposure series.

The detector also accepts regular textures as charts, so the operator picks the
chart among its candidates (thumbnails with the candidate outlined), even when
there is only one.
"""

from __future__ import annotations

from dataclasses import dataclass

from ..decoder.gox import color_checker
from . import preview_images

NO_CHART_SELECTED = "The operator found no chart among the detected candidates"


@dataclass(frozen=True)
class CandidateView:
    number: int  # what choose() takes
    label: str
    image: str  # preview_images URL


def register(stem: str, offered: list[tuple[str, color_checker.ChartCandidate]]) -> tuple[CandidateView, ...]:
    """Publish (label, candidate) thumbnails, numbered from 1 in the given order."""
    return tuple(CandidateView(number, f"{label} · OpenCV cost {candidate.cost:.3g}",
                               preview_images.put(candidate.thumbnail_png, f"{stem}_candidate{number}.png"))
                 for number, (label, candidate) in enumerate(offered, 1))


def release(candidates: tuple[CandidateView, ...]) -> None:
    preview_images.release(*(candidate.image for candidate in candidates))


def check_key(chart_key: str) -> None:
    if not isinstance(chart_key, str) or chart_key not in color_checker.CHARTS:
        raise ValueError("Explicitly select MCC24, SG140 or VINYL18 before capturing a reference")
