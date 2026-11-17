"""FX10 live focus assist: stream the workbench camera and measure the edge target's LSF FWHM.

``fx10_snapshot --stream-s`` uses the freerun snapshot profile (SensorSync off) with the
workbench camera, exposure, binning and frame rate as command-line overrides, and writes
about 0.25 s of lines per RAW segment. At most every ANALYSIS_INTERVAL_S this service
averages the newest frames (about AVERAGE_S of them, at most MAX_AVERAGE_FRAMES) and
measures the whole line with decoder.fx10.focus; services.focus_stream runs the tool,
prunes the segments behind the newest frames and ends the stream (Stop, the header's
Stop tool, the duration limit, a failure, or no Camera Tools page polling).

The state below belongs to the event loop. Worker threads receive immutable arguments
and return a FocusSample; pages read a FocusView copy.
"""

from __future__ import annotations

import asyncio
import collections
import math
import statistics
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from ..constants import BIN_FX10_SNAPSHOT, FX10_FOCUS_DIR, FX10_SNAPSHOT_CONFIG
from ..decoder import fx10
from ..decoder.ebus import recording as raw_recording
from ..decoder.fx10 import focus, recording as fx10_recording
from ..state import STATE
from . import camera_operations, config_store, focus_stream
from .focus_stream import MAX_STREAM_S, StreamResult

ANALYSIS_INTERVAL_S = 0.25
AVERAGE_S = 0.2
MAX_AVERAGE_FRAMES = 16
MEDIAN_UPDATES = 3
HISTORY_POINTS = 600


@dataclass(frozen=True)
class FocusRequest:
    """The workbench values, already checked like Take snapshot does."""

    target_ip: str
    exposure_ms: float
    spatial_binning: int
    spectral_binning: int
    frame_rate_hz: float


def average_frames(frame_rate_hz: float) -> int:
    """Frames averaged per measurement: about AVERAGE_S of the stream, 1..MAX_AVERAGE_FRAMES."""
    return min(MAX_AVERAGE_FRAMES, max(1, round(frame_rate_hz * AVERAGE_S)))


@dataclass(frozen=True)
class FocusView:
    phase: str  # idle | starting | live | stopping
    request: FocusRequest | None
    message: str  # why the last stream ended, or the newest frame read problem
    message_is_error: bool
    raw_output: str  # tool output of the last finished stream
    version: int  # changes whenever anything below changes
    frame_size: tuple[int, int] | None  # (samples, bands)
    frames: int | None  # averaged in the newest measurement
    measurement: focus.Measurement | None
    current: float | None  # median FWHM of the last MEDIAN_UPDATES measurements, px
    best: float | None  # session minimum of current
    best_groups: tuple[float | None, ...]  # session minimum FWHM per band group
    clipped_pct: float | None
    rate: float  # measurements per second
    history: tuple[tuple[float, float], ...]  # (s since the best was reset, current)
    image_url: str
    loupe_url: str
    loupe: focus.Window | None

    @property
    def pct(self) -> float | None:
        """Lower FWHM is sharper: 100 % at the session best."""
        if self.current is None or self.best is None:
            return None
        return 100.0 * self.best / self.current


class _State:
    def __init__(self) -> None:
        self.phase = "idle"
        self.request: FocusRequest | None = None
        self.message = ""
        self.message_is_error = False
        self.raw_output = ""
        self.version = 0
        self.task: asyncio.Task | None = None
        self.stop_requested = False
        self.last_touch = 0.0
        self.loupe_center: tuple[float, float] | None = None  # None: frame centre
        self.images = focus_stream.Images()
        self.image_url = self.loupe_url = ""
        self.reset_stream()

    def reset_stream(self) -> None:
        self.camera_dir: Path | None = None
        self.capture: fx10.Capture | None = None
        self.last_seq: int | None = None
        self.read_errors = 0
        self.analysed_at = 0.0
        self.frame_size: tuple[int, int] | None = None
        self.frames: int | None = None
        self.measurement: focus.Measurement | None = None
        self.clipped_pct: float | None = None
        self.loupe: focus.Window | None = None
        self.rate = 0.0
        self.update_t: float | None = None
        self.reset_best()

    def reset_best(self) -> None:
        self.fwhms: collections.deque[float] = collections.deque(maxlen=MEDIAN_UPDATES)
        self.current: float | None = None
        self.best: float | None = None
        self.best_groups: np.ndarray | None = None
        self.history: collections.deque[tuple[float, float]] = collections.deque(maxlen=HISTORY_POINTS)
        self.t0: float | None = None


_s = _State()


def _changed() -> None:
    _s.version += 1


def view() -> FocusView:
    best_groups = () if _s.best_groups is None else tuple(
        None if math.isnan(value) else float(value) for value in _s.best_groups)
    return FocusView(
        phase=_s.phase, request=_s.request, message=_s.message, message_is_error=_s.message_is_error,
        raw_output=_s.raw_output, version=_s.version, frame_size=_s.frame_size, frames=_s.frames,
        measurement=_s.measurement, current=_s.current, best=_s.best, best_groups=best_groups,
        clipped_pct=_s.clipped_pct, rate=_s.rate, history=tuple(_s.history),
        image_url=_s.image_url, loupe_url=_s.loupe_url, loupe=_s.loupe,
    )


def touch() -> None:
    """Called by every open Camera Tools page; a stream nobody polls is stopped."""
    _s.last_touch = time.monotonic()


def set_loupe_center(x: float, y: float) -> None:
    """Move the magnifier (frame pixels: x = sample, y = band); the FWHM does not depend on it."""
    if _s.frame_size is None:
        return
    focus.loupe_window(*_s.frame_size, (x, y))  # validates the point
    _s.loupe_center = (float(x), float(y))
    _changed()


def reset_best() -> None:
    _s.reset_best()
    _changed()


def start(request: FocusRequest) -> None:
    """Reserve nothing yet; the supervised task takes the camera reservation."""
    if _s.phase != "idle":
        raise RuntimeError("Live focus is already running")
    reason = camera_operations.guard_reason("fx10")
    if reason or STATE.snapshot_busy:
        raise camera_operations.CameraBusyError(reason or "Another camera tool is still running")
    _release_images()
    _s.reset_stream()
    _s.phase, _s.request = "starting", request
    _s.message, _s.message_is_error, _s.raw_output = "", False, ""
    _s.stop_requested = False
    touch()
    _changed()
    _s.task = asyncio.create_task(_supervise(request))
    _s.task.add_done_callback(lambda task: None if task.cancelled() else task.exception())


def stop() -> None:
    if _s.phase in ("starting", "live"):
        _s.stop_requested = True
        _s.phase = "stopping"
        _changed()


async def _supervise(request: FocusRequest) -> None:
    result = StreamResult(False, "Live focus was interrupted")
    try:
        result = await camera_operations.run("fx10", lambda: _stream(request), kind="fx10-focus")
    except Exception as error:  # reservation refused, launch failed, or exit not confirmed
        result = StreamResult(False, f"Live focus did not complete: {error}")
    finally:
        _s.message, _s.message_is_error = result.reason, not result.ok
        if result.raw_output:
            _s.raw_output = result.raw_output
        _s.phase = "idle"
        _changed()


async def _stream(request: FocusRequest) -> StreamResult:
    """Run inside the camera reservation: launch, follow, then stop the tool."""
    output = await asyncio.to_thread(focus_stream.fresh_output, FX10_FOCUS_DIR)
    config = config_store.checked_snapshot_path(FX10_SNAPSHOT_CONFIG)
    argv = [
        str(BIN_FX10_SNAPSHOT), "--config", str(config),
        "--out", str(output), "--ip", request.target_ip,
        "--exposure-ms", str(request.exposure_ms),
        "--spatial-binning", str(request.spatial_binning), "--spectral-binning", str(request.spectral_binning),
        "--fps", str(request.frame_rate_hz), "--stream-s", f"{MAX_STREAM_S:g}",
    ]
    return await focus_stream.run(argv, output, poll=_poll, stop_reason=_stop_reason, on_stopping=_on_stopping)


def _stop_reason() -> str:
    return "Stopped" if _s.stop_requested else focus_stream.no_page_reason(_s.last_touch)


def _on_stopping() -> None:
    _s.phase = "stopping"
    _changed()


async def _poll(output: Path) -> str:
    """Measure the newest frames if due; return a reason to end the stream, else ""."""
    if _s.camera_dir is None:
        _s.camera_dir = await asyncio.to_thread(focus_stream.camera_directory, output)
        if _s.camera_dir is None:
            return ""
    now = time.monotonic()
    if now - _s.analysed_at < ANALYSIS_INTERVAL_S:
        return ""
    _s.analysed_at = now
    count = average_frames(_s.request.frame_rate_hz)
    center = _s.loupe_center
    try:
        if _s.capture is None:  # device.json is written before the first segment
            _s.capture = await asyncio.to_thread(fx10.load_capture, _s.camera_dir)
        sample = await asyncio.to_thread(_read_newest, _s.camera_dir, _s.capture, _s.last_seq, count, center)
    except focus_stream.READ_ERRORS as error:
        _s.read_errors += 1
        _s.message, _s.message_is_error = f"Cannot analyse the newest frames: {error}", True
        _changed()
        return _s.message if _s.read_errors >= focus_stream.MAX_READ_ERRORS else ""
    if sample is not None:
        _publish(sample, time.monotonic())
    return ""


def _publish(sample: focus.FocusSample, now: float) -> None:
    _s.read_errors = 0
    if _s.message_is_error:
        _s.message, _s.message_is_error = "", False
    _s.last_seq = sample.seq
    _s.frame_size, _s.frames = (sample.samples, sample.bands), sample.frames
    _s.measurement, _s.clipped_pct, _s.loupe = sample.measurement, sample.clipped_pct, sample.loupe
    if _s.update_t is not None and now > _s.update_t:
        rate = 1.0 / (now - _s.update_t)
        _s.rate = rate if not _s.rate else 0.8 * _s.rate + 0.2 * rate
    _s.update_t = now
    if _s.phase == "starting":
        _s.phase = "live"
    _s.image_url = _s.images.replace(_s.image_url, sample.image_png, f"fx10_focus_frame_{sample.seq}.png")
    _s.loupe_url = _s.images.replace(_s.loupe_url, sample.loupe_png, f"fx10_focus_loupe_{sample.seq}.png")
    fwhm = sample.measurement.fwhm_px
    if fwhm is None:
        _s.fwhms.clear()
        _s.current = None
    else:
        _s.fwhms.append(fwhm)
        _s.current = statistics.median(_s.fwhms)
        _s.best = _s.current if _s.best is None else min(_s.best, _s.current)
        groups = np.array([math.nan if value is None else value for value in sample.measurement.group_fwhm_px])
        same = _s.best_groups is not None and _s.best_groups.shape == groups.shape
        _s.best_groups = np.fmin(_s.best_groups, groups) if same else groups
        _s.t0 = now if _s.t0 is None else _s.t0
        _s.history.append((round(now - _s.t0, 2), _s.current))
    _changed()


def _release_images() -> None:
    _s.images.release(_s.image_url, _s.loupe_url)
    _s.image_url = _s.loupe_url = ""


def _read_newest(camera_dir: Path, capture: fx10.Capture, last_seq: int | None, count: int,
                 center) -> focus.FocusSample | None:
    """Worker thread: measure the newest ``count`` committed frames unless none is new."""
    try:
        records = raw_recording.latest_records(camera_dir, count)
        if not records or records[-1].sequence == last_seq:
            return None
        frames = np.stack([fx10_recording.decode_frame(record, capture) for record in records])
        return focus.analyze(frames, capture, records[-1].sequence, center)
    finally:
        focus_stream.prune_segments(camera_dir)
