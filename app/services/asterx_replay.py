"""Read-only, per-page history playback of the two AsteRx live CSV streams.

The timeline uses recorded host_unix_ns, retaining integer nanoseconds while
indexing/merging. Files must remain unchanged after indexing. No device APIs,
recording state, acquisition configuration or raw data are modified.
"""

from __future__ import annotations

import asyncio
import bisect
import math
import os
import queue
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from ..constants import REPO_ROOT
from . import runtime
from .asterx_live import AsterxTelemetry, CSV_COLUMNS, MAX_ROW_BYTES, TRACK_MAX, TRACK_MIN_STEP_DEG, csv_fields, csv_header

_IO_SLOTS = asyncio.Semaphore(2)
_MAX_CHECKPOINTS = 4096
_MAX_UNIX_NS = 253402300799999999999  # UTC year 9999


class ReplayCancelled(RuntimeError):
    pass


def _check_cancel(cancel: threading.Event) -> None:
    if cancel.is_set():
        raise ReplayCancelled("History operation was cancelled")


def _fingerprint(stat) -> tuple[int, int, int, int]:
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def _sources(value: str) -> tuple[Path, ...]:
    path = Path(value.strip()).expanduser()
    if not value.strip():
        raise ValueError("Enter a session directory, AsteRx CSV directory, or live CSV file")
    if not path.is_absolute():
        path = REPO_ROOT / path
    if not path.exists() and runtime.is_docker() and str(path).startswith("/workspace/"):
        path = runtime.to_host_path(str(path))
    path = path.resolve()
    if path.is_file():
        if path.name not in CSV_COLUMNS:
            raise ValueError("Supported files: live_insnavgeod.csv and live_receiverstatus.csv")
        return (path,)
    if path.is_dir():
        for directory in (path, path / "asterx", path / "raw" / "asterx"):
            files = tuple(directory / name for name in CSV_COLUMNS if (directory / name).is_file())
            if files:
                return files
    raise ValueError("No supported AsteRx live CSV found; select a session or its raw/asterx directory")


def _record(raw: bytes, columns: tuple[str, ...]) -> tuple[int, tuple[str, ...]] | None:
    fields = csv_fields(raw, columns)
    if fields is None:
        return None
    try:
        stamp = int(fields[columns.index("host_unix_ns")])
    except ValueError:
        return None
    return (stamp, fields) if 0 < stamp <= _MAX_UNIX_NS else None


@dataclass(frozen=True)
class StreamIndex:
    path: Path
    fingerprint: tuple[int, int, int, int]
    columns: tuple[str, ...]
    checkpoints: tuple[tuple[int, int], ...]  # (host timestamp, byte offset)
    first_ns: int
    last_ns: int
    rows: int
    skipped: int
    partial: int


@dataclass(frozen=True)
class HistoryIndex:
    streams: tuple[StreamIndex, ...]
    first_ns: int
    last_ns: int
    track: tuple[tuple[int, float, float], ...]  # bounded route overview
    gaps: tuple[tuple[str, int, int], ...] = ()
    gap_count: int = 0

    @property
    def duration_s(self) -> float:
        return (self.last_ns - self.first_ns) / 1e9


def _index(value: str, cancel: threading.Event, report=lambda value: None) -> HistoryIndex:
    streams = []
    track = []
    gaps, gap_count = [], 0
    paths = _sources(value)
    total_bytes = sum(path.stat().st_size for path in paths)
    processed = 0
    for path in paths:
        _check_cancel(cancel)
        with path.open("rb") as stream:
            before = _fingerprint(os.fstat(stream.fileno()))
            header = stream.readline(MAX_ROW_BYTES + 1)
            if len(header) > MAX_ROW_BYTES or not header.endswith(b"\n"):
                raise ValueError(f"{path.name}: incomplete or oversized CSV header")
            columns = csv_header(path.name, header)
            checkpoints = []
            stride = 4096
            rows = skipped = partial = 0
            first = last = 0
            next_report = 0
            while True:
                _check_cancel(cancel)
                offset = stream.tell()
                if offset >= next_report:
                    report((path.name, processed + offset, total_bytes))
                    next_report = offset + 1024 * 1024
                raw = stream.readline(MAX_ROW_BYTES + 1)
                if not raw:
                    break
                if len(raw) > MAX_ROW_BYTES:
                    raise ValueError(f"{path.name}: oversized row at byte {offset}")
                if not raw.endswith(b"\n"):
                    partial += 1
                    break
                record = _record(raw, columns)
                if record is None:
                    skipped += 1
                    continue
                stamp, fields = record
                if rows and stamp < last:
                    raise ValueError(f"{path.name}: recorded host time moves backwards at byte {offset}; "
                                     "automatic time-based playback is unavailable")
                if rows and stamp - last > 2_000_000_000:
                    gap_count += 1
                    if len(gaps) < 512:
                        gaps.append((path.name, last, stamp))
                if rows % stride == 0:
                    checkpoints.append((stamp, offset))
                    if len(checkpoints) > _MAX_CHECKPOINTS:
                        checkpoints = checkpoints[::2]
                        stride *= 2
                first = first or stamp
                last = stamp
                rows += 1
                if path.name == "live_insnavgeod.csv":
                    try:
                        lat = float(fields[columns.index("lat_deg")])
                        lon = float(fields[columns.index("lon_deg")])
                    except ValueError:
                        continue
                    if not (math.isfinite(lat) and math.isfinite(lon) and -90 <= lat <= 90 and -180 <= lon <= 180):
                        continue
                    if track and abs(track[-1][1] - lat) < TRACK_MIN_STEP_DEG and abs(track[-1][2] - lon) < TRACK_MIN_STEP_DEG:
                        continue
                    track.append((stamp, lat, lon))
                    if len(track) > TRACK_MAX:
                        track = track[::2]
            if before != _fingerprint(os.fstat(stream.fileno())) or before != _fingerprint(path.stat()):
                raise ValueError(f"{path.name}: file changed during indexing; choose a completed recording")
        streams.append(StreamIndex(path, before, columns, tuple(checkpoints), first, last, rows, skipped, partial))
        processed += before[2]
        report((path.name, processed, total_bytes))
    valid = [stream for stream in streams if stream.rows]
    if not valid:
        raise ValueError("No complete rows with valid host timestamps were found")
    return HistoryIndex(tuple(streams), min(s.first_ns for s in valid), max(s.last_ns for s in valid),
                        tuple(track), tuple(sorted(gaps, key=lambda item: item[1])), gap_count)


@dataclass(frozen=True)
class _Cursor:
    target_ns: int
    offset: int
    fields: tuple[str, ...] | None


def _frame(index: HistoryIndex, target_ns: int, previous: dict[Path, _Cursor],
           cancel: threading.Event) -> dict[Path, _Cursor]:
    cursors = {}
    for item in index.streams:
        _check_cancel(cancel)
        if _fingerprint(item.path.stat()) != item.fingerprint:
            raise ValueError(f"{item.path.name}: file changed after loading; reload the history")
        if not item.rows or target_ns < item.first_ns:
            cursors[item.path] = _Cursor(target_ns, 0, None)
            continue
        checkpoint = bisect.bisect_right(item.checkpoints, (target_ns, item.fingerprint[2])) - 1
        offset = item.checkpoints[max(0, checkpoint)][1]
        fields = None
        old = previous.get(item.path)
        if old is not None and old.target_ns <= target_ns and old.offset >= offset:
            offset, fields = old.offset, old.fields
        with item.path.open("rb") as stream:
            if _fingerprint(os.fstat(stream.fileno())) != item.fingerprint:
                raise ValueError(f"{item.path.name}: file was replaced; reload the history")
            stream.seek(offset)
            while True:
                _check_cancel(cancel)
                row_offset = stream.tell()
                raw = stream.readline(MAX_ROW_BYTES + 1)
                if not raw or not raw.endswith(b"\n"):
                    offset = row_offset
                    break
                if len(raw) > MAX_ROW_BYTES:
                    raise ValueError(f"{item.path.name}: oversized row")
                record = _record(raw, item.columns)
                if record is not None:
                    stamp, next_fields = record
                    if stamp > target_ns:
                        offset = row_offset
                        break
                    fields = next_fields
                offset = stream.tell()
            if _fingerprint(os.fstat(stream.fileno())) != item.fingerprint or _fingerprint(item.path.stat()) != item.fingerprint:
                raise ValueError(f"{item.path.name}: file changed during playback; reload the history")
        cursors[item.path] = _Cursor(target_ns, offset, fields)
    return cursors


class HistoryPlayer:
    """One instance per browser page; background I/O cannot mutate live state."""

    def __init__(self) -> None:
        self.data = AsterxTelemetry()
        self.index: HistoryIndex | None = None
        self.position_s = 0.0
        self._timestamp_ns: int | None = None
        self.speed = 1.0
        self.playing = False
        self.busy = False
        self.error = ""
        self._closed = False
        self._generation = 0
        self._cancel = threading.Event()
        self._task: asyncio.Task | None = None
        self._cursors: dict[Path, _Cursor] = {}
        self._anchor_s = 0.0
        self._anchor_mono = 0.0
        self._progress_queue = queue.Queue(maxsize=1)
        self._progress = ("", 0, 0)

    def _report_progress(self, value) -> None:
        try:
            self._progress_queue.get_nowait()
        except queue.Empty:
            pass
        self._progress_queue.put_nowait(value)

    @property
    def progress(self) -> tuple[str, int, int]:
        try:
            self._progress = self._progress_queue.get_nowait()
        except queue.Empty:
            pass
        return self._progress

    @property
    def timestamp_ns(self) -> int | None:
        return self._timestamp_ns

    def pause(self) -> None:
        if self.playing:
            self._generation += 1  # discard a pending playback frame
        self.playing = False

    def play(self) -> None:
        if self.index is None or self.busy or self._closed:
            return
        self.playing = self.position_s < self.index.duration_s
        self._anchor_s, self._anchor_mono = self.position_s, time.monotonic()

    def set_speed(self, speed: float) -> None:
        if speed not in (0.25, 0.5, 1, 2, 4, 8):
            raise ValueError("Unsupported playback speed")
        resume = self.playing
        self.pause()
        self.speed = speed
        if resume:
            self.playing = True
            self._anchor_s, self._anchor_mono = self.position_s, time.monotonic()

    def cancel_read(self) -> None:
        self.pause()
        self._generation += 1
        self._cancel.set()
        self.error = "History read cancelled"

    def close(self) -> None:
        self.pause()
        self._closed = True
        self._generation += 1
        self._cancel.set()  # the worker exits without touching this page again

    async def _io(self, read, publish) -> None:
        if self.busy or self._closed:
            raise RuntimeError("A history read is still in progress")
        self.busy = True
        generation = self._generation
        cancel = self._cancel = threading.Event()

        async def execute() -> None:
            try:
                async with _IO_SLOTS:
                    _check_cancel(cancel)
                    result = await asyncio.to_thread(read, cancel)
                if not self._closed and generation == self._generation:
                    publish(result)
                    self.error = ""
            except ReplayCancelled:
                pass
            except Exception as e:
                if not self._closed and generation == self._generation:
                    self.error = str(e)
                    self.playing = False
                raise
            finally:
                self.busy = False

        self._task = asyncio.create_task(execute())
        self._task.add_done_callback(lambda task: None if task.cancelled() else task.exception())
        await asyncio.shield(self._task)

    def _publish(self, index: HistoryIndex, target_ns: int, cursors: dict[Path, _Cursor]) -> None:
        self.index, self._cursors = index, cursors
        self._timestamp_ns = target_ns
        self.position_s = (target_ns - index.first_ns) / 1e9
        self.data.reset()
        for item in index.streams:
            fields = cursors[item.path].fields
            if fields is not None:
                self.data.ingest(item.path.name, item.columns, fields)
        self.data.track = [(lat, lon) for stamp, lat, lon in index.track if stamp <= target_ns]
        if self.data.ins is not None:
            self.data._track_append(self.data.ins.lat_deg, self.data.ins.lon_deg)
        self.data.track_version += 1
        if target_ns >= index.last_ns:
            self.playing = False

    async def load(self, value: str) -> None:
        if self.busy or self._closed:
            raise RuntimeError("Wait for the current history operation to finish")
        self.pause()
        self._generation += 1
        self.index = None
        self.position_s = 0.0
        self._timestamp_ns = None
        self.data.reset()
        self._cursors = {}
        self.error = ""

        def read(cancel):
            self._report_progress(("Indexing", 0, 0))
            index = _index(value, cancel, self._report_progress)
            return index, _frame(index, index.first_ns, {}, cancel)

        await self._io(read, lambda result: self._publish(result[0], result[0].first_ns, result[1]))

    async def seek(self, seconds: float) -> None:
        if self.index is None or not math.isfinite(seconds):
            raise ValueError("Load a history and enter a valid playback position")
        if self.busy:
            raise RuntimeError("Wait for the current history operation to finish")
        self.pause()
        await self._show(min(self.index.duration_s, max(0.0, seconds)))

    async def _show(self, seconds: float) -> None:
        index, cursors = self.index, self._cursors
        if index is None:
            return
        target = index.last_ns if seconds >= index.duration_s else index.first_ns + round(seconds * 1e9)
        await self._io(lambda cancel: _frame(index, target, cursors, cancel),
                       lambda result: self._publish(index, target, result))

    async def tick(self) -> None:
        if self.index is None or not self.playing or self.busy or self._closed:
            return
        target = self._anchor_s + (time.monotonic() - self._anchor_mono) * self.speed
        await self._show(min(self.index.duration_s, target))
