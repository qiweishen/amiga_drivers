"""On-demand preview of the RUNNING acquisition session (Data Live page).

Nothing polls: each fetch is a single read of the files the drivers are
appending to right now, restricted to committed bytes.

- GoX: newest complete ebus-raw image.
- FX10: raw DN statistics over valid ebus-raw frames.
  Device/profile metadata fixes the interpretation. SensorSync edges require a verified
  association before they can be treated as GNSS camera-line timestamps.
"""

from __future__ import annotations

import asyncio
import cv2
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..decoder import fx10
from ..decoder.gox import preview as gox_preview, recording as gox_recording
from ..state import STATE, ProcState
from . import preview_images

_WINDOW_S = 1.0  # frame-count window estimated from the configured rate
_FX10_STALE_S = 3.0  # age of the newest frame's host receive timestamp


@dataclass
class GoxLive:
    ok: bool
    reason: str = ""
    # preview_images URL paths: 8-bit screen preview, and the on-demand
    # full-resolution 16-bit PNG re-decoded from the same committed record.
    image_url: str = ""
    full_image_url: str = ""
    camera: str = ""
    seq: int = 0
    width: int = 0
    height: int = 0
    pixel_format: str = ""
    age_s: float = 0.0  # click time minus the frame's host timestamp
    request: PreviewRequest | None = None


@dataclass
class Fx10Live:
    ok: bool
    reason: str = ""
    wavelengths_nm: list[float] = field(default_factory=list)
    spectrum_dn: dict[str, list[float]] = field(default_factory=dict)
    full_scale: int = 0
    pixel_format: str = ""
    frames: int = 0
    bands: int = 0
    samples: int = 0
    age_s: float = 0.0  # host receive time
    request: PreviewRequest | None = None


# --- shared ------------------------------------------------------------------

@dataclass(frozen=True)
class PreviewRequest:
    session: Path
    generation: int


class PreviewUnavailable(RuntimeError):
    pass


class PreviewObsolete(PreviewUnavailable):
    pass


_preview_task: asyncio.Task | None = None


def busy() -> bool:
    return _preview_task is not None and not _preview_task.done()


def is_current(request: PreviewRequest | None) -> bool:
    return (request is not None and STATE.process_state is ProcState.RUNNING
            and STATE.ownership_verified and not STATE.control_uncertain
            and request.generation == STATE.session_generation
            and request.session == STATE.active_session)


async def _fetch(driver: str, decode):
    global _preview_task
    if busy():
        raise PreviewUnavailable("Another live preview is still being decoded; wait for it to finish")
    if (STATE.process_state is not ProcState.RUNNING or STATE.active_session is None
            or not STATE.ownership_verified or STATE.control_uncertain):
        raise PreviewUnavailable("There is no verified running session to preview")
    if not STATE.enables_at_start.get(driver, False):
        raise PreviewUnavailable(f"{driver} is not enabled in this recording")
    request = PreviewRequest(STATE.active_session, STATE.session_generation)

    async def decode_request():
        # Pass a fixed path into the worker. It must never switch sessions by
        # rereading global UI state halfway through a file operation.
        result = await asyncio.to_thread(decode, request.session)
        result.request = request
        return result

    _preview_task = asyncio.create_task(decode_request())
    _preview_task.add_done_callback(lambda task: None if task.cancelled() else task.exception())
    # Cancelling a page await does not stop a worker thread. Keep the global
    # reservation until that worker really finishes; do not queue more work.
    result = await asyncio.shield(_preview_task)
    if not is_current(request):
        raise PreviewObsolete("The recording changed while the preview was being decoded")
    return result


async def fetch_gox() -> GoxLive:
    return await _fetch("gox", gox_latest_frame)


async def fetch_fx10() -> Fx10Live:
    return await _fetch("fx10", fx10_spectrum)


def gox_latest_frame(session: Path) -> GoxLive:
    """Newest committed frame; shared raw/header/PixelFormat interpretation."""
    root = session / "raw" / "gox"
    try:
        cameras = sorted(d for d in root.iterdir() if d.is_dir()) if root.is_dir() else []
        for cam_dir in cameras:
            image = gox_recording.latest_frame(cam_dir)
            frame = image.record
            stem = f"gox_live_{session.name}_{cam_dir.name}_seq{frame.seq}"
            image_url = preview_images.put(gox_preview.live_display_png(image), f"{stem}_screen.png")
            full_image_url = preview_images.lazy(lambda: gox_preview.record_full_png16(frame),
                                                 f"{stem}_16bit.png")
            return GoxLive(True, image_url=image_url, full_image_url=full_image_url,
                           camera=cam_dir.name, seq=frame.seq, width=frame.w, height=frame.h,
                           pixel_format=image.pixel_format, age_s=max(0.0, time.time() - frame.hrt / 1e9))
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError, cv2.error) as error:
        return GoxLive(False, reason=f"Cannot read committed GoX frame: {error}")
    return GoxLive(False, reason=f"No camera data under {root} yet")


# --- FX10 --------------------------------------------------------------------

def fx10_spectrum(session: Path) -> Fx10Live:
    """Bounded frame-count window at expected rate; not a timestamp-filtered second."""
    root = session / "raw" / "fx10"
    try:
        directories = sorted(d for d in root.iterdir() if d.is_dir()) if root.is_dir() else []
        if not directories:
            return Fx10Live(False, reason=f"No FX10 session under {root} yet")
        capture = fx10.load_capture(directories[-1])
        window = max(1, round(capture.expected_frame_rate_hz * _WINDOW_S))
        cube, host_timestamp = fx10.raw_tail_cube(directories[-1], capture, window)
        age = max(0.0, time.time() - host_timestamp / 1e9)
        if age > _FX10_STALE_S:
            return Fx10Live(False, reason=f"Newest valid raw image is {age:.1f} s old")
        values, _, _ = fx10.spectrum(cube, capture.full_scale)
        return Fx10Live(True, wavelengths_nm=list(capture.wavelengths_nm), spectrum_dn=values,
                        full_scale=capture.full_scale, pixel_format=capture.pixel_format,
                        frames=cube.shape[0], bands=capture.bands, samples=capture.samples, age_s=age)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError) as error:
        return Fx10Live(False, reason=f"Cannot read committed FX10 data (retry after rotation): {error}")
