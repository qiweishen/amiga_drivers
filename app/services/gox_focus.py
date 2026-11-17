"""GOX live focus assist: stream the workbench camera and score focus on its newest frame.

``jai_snapshot --stream-s`` uses the freerun snapshot profile (PTP and SensorSync
off; the workbench camera, exposure and gain as command-line overrides) but in
8-bit BayerRG8 and always at the highest frame rate the exposure allows under
the 9.078 Hz focus ceiling (timing.GOX_MAX_FOCUS_FRAME_RATE_HZ). It writes one
RAW record per segment. This service follows the newest committed record and
scores it with decoder.gox.focus; services.focus_stream runs the tool, prunes
the segments behind the newest frames and ends the stream (Stop, the header's
Stop tool, the duration limit, a failure, or no Camera Tools page polling).

The state below belongs to the event loop. Worker threads receive immutable
arguments and return a FocusSample; pages read a FocusView copy.
"""

from __future__ import annotations

import asyncio
import collections
import statistics
import time
from dataclasses import dataclass
from pathlib import Path

from ..constants import BIN_SNAPSHOT, FOCUS_DIR, SNAPSHOT_CONFIG
from ..decoder.ebus import recording as raw_recording
from ..decoder.gox import focus
from ..state import STATE
from . import camera_operations, config_store, focus_stream, timing
from .focus_stream import MAX_STREAM_S, StreamResult

OVERVIEW_INTERVAL_S = 1.0
MEDIAN_FRAMES = 3
HISTORY_POINTS = 600
DEFAULT_HALF_PX = 200


@dataclass(frozen=True)
class FocusRequest:
    target_ip: str
    exposure_ms: float
    gain: float
    frame_rate_hz: float  # always timing.gox_max_frame_rate_hz(exposure_ms, MAX_FOCUS_FRAME_RATE_HZ)


def request_for(target_ip: str, exposure_ms: float, gain: float) -> FocusRequest:
    """The focus stream runs at the highest rate at which the exposure uses <= 90 % of the period."""
    exposure_ms = timing.gox_check_exposure(exposure_ms)
    rate = timing.gox_max_frame_rate_hz(exposure_ms, timing.GOX_MAX_FOCUS_FRAME_RATE_HZ)
    return FocusRequest(target_ip, exposure_ms, float(gain), rate)


@dataclass(frozen=True)
class FocusView:
    phase: str  # idle | starting | live | stopping
    request: FocusRequest | None
    message: str  # why the last stream ended, or the newest frame read problem
    message_is_error: bool
    raw_output: str  # tool output of the last finished stream
    version: int  # changes whenever anything below changes
    roi_half: int
    roi: focus.Roi | None  # the window the next frame is scored on
    frame_size: tuple[int, int] | None
    seq: int | None
    score: float | None  # newest frame, unsmoothed
    current: float | None  # median of the last MEDIAN_FRAMES scores
    peak: float | None
    signal_dn: float | None
    clipped_pct: float | None
    fps: float  # frames analysed per second
    history: tuple[tuple[float, float], ...]  # (s since the peak was reset, current)
    overview_url: str
    overview_scale: tuple[float, float]
    loupe_url: str
    loupe_origin: tuple[int, int]

    @property
    def pct(self) -> float | None:
        if self.current is None or not self.peak:
            return None
        return 100.0 * self.current / self.peak


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
        self.roi_center: tuple[float, float] | None = None  # None: frame centre
        self.roi_half = DEFAULT_HALF_PX
        self.images = focus_stream.Images()
        self.overview_url = self.loupe_url = ""
        self.reset_stream()

    def reset_stream(self) -> None:
        self.camera_dir: Path | None = None
        self.last_seq: int | None = None
        self.read_errors = 0
        self.frame_size: tuple[int, int] | None = None
        self.overview_at = 0.0
        self.overview_scale = (1.0, 1.0)
        self.loupe_origin = (0, 0)
        self.score: float | None = None
        self.signal_dn: float | None = None
        self.clipped_pct: float | None = None
        self.fps = 0.0
        self.frame_t: float | None = None
        self.seq: int | None = None
        self.reset_peak()

    def reset_peak(self) -> None:
        self.scores: collections.deque[float] = collections.deque(maxlen=MEDIAN_FRAMES)
        self.current: float | None = None
        self.peak: float | None = None
        self.history: collections.deque[tuple[float, float]] = collections.deque(maxlen=HISTORY_POINTS)
        self.t0: float | None = None


_s = _State()


def _changed() -> None:
    _s.version += 1


def view() -> FocusView:
    roi = (focus.roi_window(*_s.frame_size, _s.roi_center, _s.roi_half) if _s.frame_size else None)
    return FocusView(
        phase=_s.phase, request=_s.request, message=_s.message, message_is_error=_s.message_is_error,
        raw_output=_s.raw_output, version=_s.version, roi_half=_s.roi_half, roi=roi,
        frame_size=_s.frame_size, seq=_s.seq, score=_s.score, current=_s.current, peak=_s.peak,
        signal_dn=_s.signal_dn, clipped_pct=_s.clipped_pct, fps=_s.fps, history=tuple(_s.history),
        overview_url=_s.overview_url, overview_scale=_s.overview_scale,
        loupe_url=_s.loupe_url, loupe_origin=_s.loupe_origin,
    )


def touch() -> None:
    """Called by every open Camera Tools page; a stream nobody polls is stopped."""
    _s.last_touch = time.monotonic()


def set_roi_center(x: float, y: float) -> None:
    if _s.frame_size is None:
        return
    focus.roi_window(*_s.frame_size, (x, y), _s.roi_half)  # validates the point
    _s.roi_center = (float(x), float(y))
    _s.reset_peak()
    _changed()


def set_roi_half(half: int) -> None:
    half = min(max(int(half), focus.MIN_HALF_PX), focus.MAX_HALF_PX)
    if half == _s.roi_half:
        return
    _s.roi_half = half
    _s.reset_peak()
    _changed()


def reset_peak() -> None:
    _s.reset_peak()
    _changed()


def start(request: FocusRequest) -> None:
    """Reserve nothing yet; the supervised task takes the camera reservation."""
    if _s.phase != "idle":
        raise RuntimeError("Live focus is already running")
    reason = camera_operations.guard_reason("gox")
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
        result = await camera_operations.run("gox", lambda: _stream(request), kind="gox-focus")
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
    output = await asyncio.to_thread(focus_stream.fresh_output, FOCUS_DIR)
    config = config_store.checked_snapshot_path(SNAPSHOT_CONFIG)
    argv = [
        str(BIN_SNAPSHOT), "--config", str(config),
        "--out", str(output), "--ip", request.target_ip,
        "--exposure-ms", str(request.exposure_ms), "--gain", str(request.gain),
        "--frame-rate-hz", str(request.frame_rate_hz), "--stream-s", f"{MAX_STREAM_S:g}",
    ]
    return await focus_stream.run(argv, output, poll=_poll, stop_reason=_stop_reason, on_stopping=_on_stopping)


def _stop_reason() -> str:
    return "Stopped" if _s.stop_requested else focus_stream.no_page_reason(_s.last_touch)


def _on_stopping() -> None:
    _s.phase = "stopping"
    _changed()


async def _poll(output: Path) -> str:
    """Analyse the newest frame if there is one; return a reason to end the stream, else ""."""
    if _s.camera_dir is None:
        _s.camera_dir = await asyncio.to_thread(focus_stream.camera_directory, output)
        if _s.camera_dir is None:
            return ""
    now = time.monotonic()
    center, half = _s.roi_center, _s.roi_half
    overview = not _s.overview_url or now - _s.overview_at >= OVERVIEW_INTERVAL_S
    try:
        sample = await asyncio.to_thread(_read_newest, _s.camera_dir, _s.last_seq, center, half, overview)
    except focus_stream.READ_ERRORS as error:
        _s.read_errors += 1
        _s.message, _s.message_is_error = f"Cannot analyse the newest frame: {error}", True
        _changed()
        return _s.message if _s.read_errors >= focus_stream.MAX_READ_ERRORS else ""
    if sample is not None:
        _publish(sample, center, half, time.monotonic())
    return ""


def _publish(sample: focus.FocusSample, center, half: int, now: float) -> None:
    _s.read_errors = 0
    if _s.message_is_error:
        _s.message, _s.message_is_error = "", False
    _s.last_seq = _s.seq = sample.seq
    _s.frame_size = (sample.width, sample.height)
    if _s.frame_t is not None and now > _s.frame_t:
        rate = 1.0 / (now - _s.frame_t)
        _s.fps = rate if not _s.fps else 0.8 * _s.fps + 0.2 * rate
    _s.frame_t = now
    if _s.phase == "starting":
        _s.phase = "live"
    if sample.overview_png is not None:
        _s.overview_url = _s.images.replace(_s.overview_url, sample.overview_png,
                                            f"gox_focus_overview_{sample.seq}.png")
        _s.overview_scale, _s.overview_at = sample.overview_scale, now
    if (center, half) == (_s.roi_center, _s.roi_half):  # else the ROI moved during the analysis
        _s.loupe_url = _s.images.replace(_s.loupe_url, sample.loupe_png, f"gox_focus_roi_{sample.seq}.png")
        _s.loupe_origin = sample.loupe_origin
        _s.score, _s.signal_dn, _s.clipped_pct = sample.score, sample.signal_dn, sample.clipped_pct
        if sample.score is None:
            _s.scores.clear()
            _s.current = None
        else:
            _s.scores.append(sample.score)
            _s.current = statistics.median(_s.scores)
            _s.peak = _s.current if _s.peak is None else max(_s.peak, _s.current)
            _s.t0 = now if _s.t0 is None else _s.t0
            _s.history.append((round(now - _s.t0, 2), _s.current))
    _changed()


def _release_images() -> None:
    _s.images.release(_s.overview_url, _s.loupe_url)
    _s.overview_url = _s.loupe_url = ""


def _read_newest(camera_dir: Path, last_seq: int | None, center, half: int,
                 overview: bool) -> focus.FocusSample | None:
    """Worker thread: score the newest committed frame unless it was seen already."""
    try:
        record = raw_recording.newest_valid_record(camera_dir, segments=focus_stream.KEEP_SEGMENTS)
        if record is None or record.sequence == last_seq:
            return None
        return focus.analyze(record, raw_recording.read_payload(record), center, half, overview=overview)
    finally:
        focus_stream.prune_segments(camera_dir)
