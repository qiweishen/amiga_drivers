"""Read bounded batches of AsteRx telemetry; publish only on the event loop.

host_unix_ns is the host CSV recorder's clock, not a verified device sampling
time. GUI reads and history playback must never make old rows look live.
"""

from __future__ import annotations

import asyncio
import math
import os
import time
from collections.abc import Sequence
from dataclasses import dataclass, replace
from pathlib import Path

from .log_buffer import BUFFER, parse_line

POLL_S = 0.5
REPLAY_TAIL_BYTES = 256 * 1024
MAX_ROW_BYTES = 4096
TRACK_MAX = 2000
TRACK_MIN_STEP_DEG = 5e-6
STALE_S = 3.0
U32_DNU = 4294967295
U8_DNU = 255

CSV_COLUMNS = {
    "live_insnavgeod.csv": (
        "tow_ms", "host_unix_ns", "gnss_mode", "error", "info", "gnss_age_s",
        "lat_deg", "lon_deg", "height_m", "accuracy_m", "heading_deg", "pitch_deg", "roll_deg"),
    "live_receiverstatus.csv": (
        "tow_ms", "host_unix_ns", "cpu_load_pct", "up_time_s", "rx_status", "rx_error", "ext_error", "temp_c"),
}


@dataclass
class InsSnapshot:
    tow_ms: int | None
    gnss_mode: int | None
    error: int | None
    info: int | None
    gnss_age_s: float
    lat_deg: float
    lon_deg: float
    height_m: float
    accuracy_m: float
    heading_deg: float
    pitch_deg: float
    roll_deg: float
    host_unix_ns: int | None
    read_unix_ns: int
    mono: float  # GUI ingest time only; never used alone as data freshness


@dataclass
class RxStatusSnapshot:
    tow_ms: int | None
    cpu_load_pct: int | None
    up_time_s: int | None
    rx_status: int | None
    rx_error: int | None
    ext_error: int | None
    temp_c: float
    host_unix_ns: int | None
    read_unix_ns: int
    mono: float


def _f(colmap: dict[str, int], fields: Sequence[str], name: str) -> float:
    try:
        return float(fields[colmap[name]])
    except (KeyError, IndexError, ValueError):
        return float("nan")


def _i(colmap: dict[str, int], fields: Sequence[str], name: str, dnu: int | None = None) -> int | None:
    try:
        # Preserve integer nanoseconds exactly; converting through float loses bits.
        value = int(fields[colmap[name]])
    except (KeyError, IndexError, ValueError):
        return None
    return None if value == dnu else value


def sample_age_s(sample: InsSnapshot | RxStatusSnapshot, *, reference_ns: int | None = None) -> float | None:
    stamp = sample.host_unix_ns
    if stamp is None or stamp <= 0:
        return None
    if reference_ns is not None:  # history: age relative to the playback cursor
        return (reference_ns - stamp) / 1e9 if reference_ns >= stamp else None
    elapsed = time.monotonic() - sample.mono
    wall_elapsed = (time.time_ns() - sample.read_unix_ns) / 1e9
    # A wall-clock adjustment makes freshness uncertain until another row arrives.
    if abs(elapsed - wall_elapsed) > 1.0 or stamp > sample.read_unix_ns:
        return None
    return (sample.read_unix_ns - stamp) / 1e9 + max(0.0, elapsed)


def csv_header(name: str, raw: bytes) -> tuple[str, ...]:
    names = tuple(raw.decode("utf-8-sig").strip("\r\n").split(","))
    missing = set(CSV_COLUMNS[name]) - set(names)
    if missing or len(names) != len(set(names)):
        raise ValueError(f"{name}: missing or duplicate columns ({', '.join(sorted(missing))})")
    return names


def csv_fields(raw: bytes, columns: tuple[str, ...]) -> tuple[str, ...] | None:
    try:
        fields = tuple(raw.decode("utf-8").strip("\r\n").split(","))
    except UnicodeError:
        return None
    return fields if len(fields) == len(columns) else None


class AsterxTelemetry:
    """Display model shared by the live tailer and each page's history player."""

    def __init__(self) -> None:
        self.track: list[tuple[float, float]] = []
        self.ins: InsSnapshot | None = None
        self.rx: RxStatusSnapshot | None = None
        self.track_version = 0
        self.snap_version = 0

    def reset(self) -> None:
        self.track = []
        self.ins = self.rx = None
        self.track_version += 1
        self.snap_version += 1

    def _track_append(self, lat: float, lon: float) -> None:
        if not (math.isfinite(lat) and math.isfinite(lon)) or not (-90 <= lat <= 90 and -180 <= lon <= 180):
            return
        if self.track and all(abs(a - b) < TRACK_MIN_STEP_DEG for a, b in zip(self.track[-1], (lat, lon))):
            return
        self.track.append((lat, lon))
        if len(self.track) > TRACK_MAX:
            self.track = self.track[::2]
        self.track_version += 1

    def ingest(self, name: str, columns: tuple[str, ...], fields: Sequence[str]) -> None:
        colmap = {name: index for index, name in enumerate(columns)}
        clock = dict(host_unix_ns=_i(colmap, fields, "host_unix_ns"),
                     read_unix_ns=time.time_ns(), mono=time.monotonic())
        if name == "live_insnavgeod.csv":
            lat, lon = _f(colmap, fields, "lat_deg"), _f(colmap, fields, "lon_deg")
            self._track_append(lat, lon)
            self.ins = InsSnapshot(
                tow_ms=_i(colmap, fields, "tow_ms", U32_DNU),
                gnss_mode=_i(colmap, fields, "gnss_mode"), error=_i(colmap, fields, "error"),
                info=_i(colmap, fields, "info"), gnss_age_s=_f(colmap, fields, "gnss_age_s"),
                lat_deg=lat, lon_deg=lon, height_m=_f(colmap, fields, "height_m"),
                accuracy_m=_f(colmap, fields, "accuracy_m"), heading_deg=_f(colmap, fields, "heading_deg"),
                pitch_deg=_f(colmap, fields, "pitch_deg"), roll_deg=_f(colmap, fields, "roll_deg"), **clock)
        else:
            self.rx = RxStatusSnapshot(
                tow_ms=_i(colmap, fields, "tow_ms", U32_DNU),
                cpu_load_pct=_i(colmap, fields, "cpu_load_pct", U8_DNU),
                up_time_s=_i(colmap, fields, "up_time_s"), rx_status=_i(colmap, fields, "rx_status"),
                rx_error=_i(colmap, fields, "rx_error"), ext_error=_i(colmap, fields, "ext_error"),
                temp_c=_f(colmap, fields, "temp_c"), **clock)
        self.snap_version += 1


@dataclass(frozen=True)
class _Cursor:
    offset: int = 0
    columns: tuple[str, ...] = ()
    identity: tuple[int, int] | None = None
    tail_on_open: bool = False
    disabled: bool = False


def _read_batch(directory: Path, cursors: dict[str, _Cursor]):
    """Read at most 256 KiB per stream, keeping incomplete rows for the next poll."""
    updated = dict(cursors)
    rows = []
    errors = []
    reset = False
    skipped = 0
    for name, cursor in cursors.items():
        if cursor.disabled:
            continue
        try:
            with (directory / name).open("rb") as stream:
                stat = os.fstat(stream.fileno())
                identity = (stat.st_dev, stat.st_ino)
                if cursor.identity is not None and (identity != cursor.identity or stat.st_size < cursor.offset):
                    cursor = _Cursor(tail_on_open=True)
                    reset = True
                    errors.append(f"{name}: file was replaced or truncated; display was reset")
                if not cursor.columns:
                    header = stream.readline(MAX_ROW_BYTES + 1)
                    if len(header) > MAX_ROW_BYTES:
                        raise ValueError(f"{name}: oversized CSV header")
                    if not header.endswith(b"\n"):
                        continue
                    columns = csv_header(name, header)
                    offset = len(header)
                    if cursor.tail_on_open and stat.st_size - offset > REPLAY_TAIL_BYTES:
                        offset = stat.st_size - REPLAY_TAIL_BYTES
                        stream.seek(offset)
                        partial = stream.readline(MAX_ROW_BYTES + 1)
                        if len(partial) > MAX_ROW_BYTES:
                            raise ValueError(f"{name}: oversized CSV row at the tail seek")
                        offset += len(partial)
                    cursor = _Cursor(offset, columns, identity)
                stream.seek(cursor.offset)
                chunk = stream.read(REPLAY_TAIL_BYTES)
            end = chunk.rfind(b"\n") + 1
            if not end and len(chunk) == REPLAY_TAIL_BYTES:
                raise ValueError(f"{name}: CSV row exceeds the read limit")
            for raw in chunk[:end].splitlines():
                fields = csv_fields(raw, cursor.columns) if len(raw) <= MAX_ROW_BYTES else None
                if fields is None:
                    skipped += 1
                else:
                    rows.append((name, cursor.columns, fields))
            updated[name] = replace(cursor, offset=cursor.offset + end)
        except FileNotFoundError:
            continue  # disabled driver / stream not yet created
        except (ValueError, UnicodeError) as e:
            updated[name] = replace(cursor, disabled=True)
            errors.append(str(e))
        except OSError as e:
            errors.append(f"{name}: {e}")
    return updated, rows, errors, reset, skipped


class AsterxLive(AsterxTelemetry):
    def __init__(self) -> None:
        super().__init__()
        self._task: asyncio.Task | None = None
        self._gen = 0
        self.session_dir: Path | None = None
        self.error = ""
        self.skipped_rows = 0

    def start(self, session: Path, *, replay: bool) -> None:
        self.stop()
        self.session_dir = session
        self._task = asyncio.create_task(self._run(session / "raw" / "asterx", replay, self._gen))

    def stop(self) -> None:
        self._gen += 1
        if self._task is not None:
            self._task.cancel()
            self._task = None
        self.session_dir = None
        self.error = ""
        self.skipped_rows = 0
        self.reset()

    async def _run(self, directory: Path, replay: bool, gen: int) -> None:
        cursors = {name: _Cursor(tail_on_open=replay) for name in CSV_COLUMNS}
        reported: set[str] = set()
        while gen == self._gen:
            updated, rows, errors, reset, skipped = await asyncio.to_thread(_read_batch, directory, cursors)
            if gen != self._gen:
                return
            cursors = updated
            if reset:
                self.reset()
            self.skipped_rows += skipped
            for error in errors:
                if error not in reported:
                    BUFFER.append(parse_line(f"asterx live: {error}", fallback_module="gui"))
                    reported.add(error)
            if errors:
                self.error = " · ".join(errors)
            elif not any(cursor.disabled for cursor in cursors.values()):
                self.error = ""
            if self.skipped_rows:
                self.error = f"{self.error.split(' · Skipped')[0]} · Skipped malformed CSV rows: {self.skipped_rows}".strip(" ·")
            for name, columns, fields in rows:
                self.ingest(name, columns, fields)
            await asyncio.sleep(POLL_S)


LIVE = AsterxLive()
