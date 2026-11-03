"""On-demand preview of the RUNNING acquisition session (Data Live page).

Nothing polls: each fetch is a single read of the files the drivers are
appending to right now, restricted to committed bytes.

- GoX: newest complete eBUS image, or an indexed legacy jai-raw-seg frame.
- FX10: image-row statistics over valid raw frames, or committed legacy BIL
  lines. Device/profile metadata fixes the interpretation. SensorSync edges require a verified
  association before they can be treated as GNSS camera-line timestamps.
"""

from __future__ import annotations

import base64
import asyncio
import csv
import time
from dataclasses import dataclass, field
from pathlib import Path

import cv2

from gox_driver.scripts.jai_raw import pixels, recording
from ebus_driver.scripts.ebus_raw import recording as raw_recording

from ..formats import fx10
from ..state import STATE, ProcState

_WINDOW_S = 1.0  # "the last second before the click"
_FX10_STALE_S = 3.0  # .bil.part mtime older than this = the lines stopped


@dataclass
class GoxLive:
    ok: bool
    reason: str = ""
    jpeg_b64: str = ""
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
    spectrum_pct: dict[str, list[float]] = field(default_factory=dict)
    frames: int = 0
    bands: int = 0
    samples: int = 0
    age_s: float = 0.0  # raw: host receive time; legacy BIL: file mtime
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
            if raw_recording.is_session(cam_dir):
                frames = raw_recording.latest_records(cam_dir, 1)
                if not frames:
                    return GoxLive(False, reason="No complete valid raw image yet")
                frame = frames[-1]
                native, name, _ = pixels.decode_record(frame, raw_recording.read_payload(frame))
            else:
                indexes = sorted(cam_dir.glob("seg_*.idx.jsonl"))
                if not indexes:
                    continue
                rows = list(recording.index_records(indexes[-1], tail_bytes=recording.INDEX_TAIL_BYTES))
                if not rows:
                    return GoxLive(False, reason="No committed frames yet — retry after the index flush")
                frame, payload = recording.read_indexed_frame(recording.segment_for_index(indexes[-1]), rows[-1])
                native, name, _ = pixels.decode(frame.pf, frame.w, frame.h, payload)
            img8 = pixels.live_image(native, pixels.format_for_code(frame.pf))
            ok, jpg = cv2.imencode(".jpg", img8, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
            if not ok:
                return GoxLive(False, reason="JPEG encoding failed")
            return GoxLive(True, jpeg_b64=base64.b64encode(jpg.tobytes()).decode(),
                           camera=cam_dir.name, seq=frame.seq, width=frame.w, height=frame.h,
                           pixel_format=name, age_s=max(0.0, time.time() - frame.hrt / 1e9))
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
        if capture.recording_format == "ebus-raw-v1":
            window = max(1, round(capture.expected_frame_rate_hz * _WINDOW_S))
            cube, host_timestamp = fx10.raw_tail_cube(directories[-1], capture, window)
            age = max(0.0, time.time() - host_timestamp / 1e9)
            if age > _FX10_STALE_S:
                return Fx10Live(False, reason=f"Newest valid raw image is {age:.1f} s old")
            values, _, _ = fx10.spectrum(cube, capture.full_scale)
            return Fx10Live(True, wavelengths_nm=list(capture.wavelengths_nm), spectrum_pct=values,
                            frames=cube.shape[0], bands=capture.bands, samples=capture.samples, age_s=age)
        files = sorted(directories[-1].glob("segment_*.bil.part")) or sorted(directories[-1].glob("segment_*.bil"))
        if not files:
            return Fx10Live(False, reason="No open segment yet")
        data = files[-1]
        segment = fx10.open_segment(data, capture)
        stat = data.stat()
        if stat.st_size < capture.line_bytes:
            return Fx10Live(False, reason="No complete line on disk yet — retry")
        age = max(0.0, time.time() - stat.st_mtime)
        if age > _FX10_STALE_S:
            return Fx10Live(False, reason=f"No frames in the last {_WINDOW_S:.0f} s "
                                          f"(newest line is {age:.1f} s old — trigger pulses missing?)")
        window = max(1, round(capture.expected_frame_rate_hz * _WINDOW_S))
        cube = fx10.tail_cube(segment, window)
        values, _, _ = fx10.spectrum(cube, capture.full_scale)
        return Fx10Live(True, wavelengths_nm=list(capture.wavelengths_nm), spectrum_pct=values,
                        frames=cube.shape[0], bands=capture.bands, samples=capture.samples, age_s=age)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError, csv.Error) as error:
        return Fx10Live(False, reason=f"Cannot read committed FX10 data (retry after rotation): {error}")
