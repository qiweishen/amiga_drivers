"""Read a bounded, static scan track from the current AsteRx CSV telemetry."""

from __future__ import annotations

import math
import os
import threading
from dataclasses import dataclass
from pathlib import Path

from .recording import MAX_ROW_BYTES, csv_fields, csv_header

MAX_TRACK_BYTES = 512 * 1024 * 1024
MAX_TRACK_ROWS = 5_000_000
MAX_TRACK_POINTS = 10_000
MAX_TRACK_SEGMENTS = 1_000
# Display policy only: do not draw a route through an unobserved host-time gap.
TRACK_GAP_NS = 2_000_000_000


class TrackCancelled(Exception):
    pass


@dataclass(frozen=True)
class SessionTrack:
    source: Path
    segments: tuple[tuple[tuple[float, float], ...], ...]
    bounds: tuple[tuple[float, float], tuple[float, float]]
    rows: int
    valid_points: int
    displayed_points: int
    skipped_rows: int
    partial_rows: int
    breaks: int
    simplified: bool


def _check_cancel(cancel: threading.Event) -> None:
    if cancel.is_set():
        raise TrackCancelled()


def _fingerprint(stat) -> tuple[int, int, int, int]:
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def _point(raw: bytes, columns: tuple[str, ...], positions: tuple[int, int, int]):
    fields = csv_fields(raw, columns)
    if fields is None:
        return None
    try:
        stamp = int(fields[positions[0]])
        lat, lon = float(fields[positions[1]]), float(fields[positions[2]])
    except (ValueError, IndexError):
        return None
    if stamp <= 0 or not (math.isfinite(lat) and math.isfinite(lon)):
        return None
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        return None
    return stamp, (lat, lon)


def _thin(segments: list[list[tuple[float, float]]]) -> int:
    """Keep each disconnected segment's endpoints and sampled original points."""
    for index, points in enumerate(segments):
        if len(points) > 2:
            reduced = points[::2]
            if len(points) % 2 == 0:
                reduced.append(points[-1])
            segments[index] = reduced
    return sum(len(points) for points in segments)


def read_track(session_dir: Path, cancel: threading.Event) -> SessionTrack:
    """Read all rows or fail explicitly; bounds include every valid input point.

    Invalid rows, reversed host time and gaps split the route. Simplification
    never joins those segments, and only retains coordinates present in the CSV.
    The file must remain unchanged throughout the read.
    """
    source = Path(session_dir) / "raw" / "asterx" / "live_insnavgeod.csv"
    segments: list[list[tuple[float, float]]] = []
    rows = valid = displayed = skipped = partial = breaks = 0
    simplified = False
    previous_ns = None
    connected = False
    south = west = math.inf
    north = east = -math.inf
    _check_cancel(cancel)
    with source.open("rb") as stream:
        initial = _fingerprint(os.fstat(stream.fileno()))
        if initial[2] > MAX_TRACK_BYTES:
            raise ValueError("Track CSV exceeds the 512 MiB history display limit; no partial range is shown")
        header = stream.readline(MAX_ROW_BYTES + 1)
        if len(header) > MAX_ROW_BYTES or not header.endswith(b"\n"):
            raise ValueError("Incomplete or oversized AsteRx track header")
        columns = csv_header(source.name, header)
        positions = tuple(columns.index(name) for name in ("host_unix_ns", "lat_deg", "lon_deg"))
        while True:
            _check_cancel(cancel)
            raw = stream.readline(MAX_ROW_BYTES + 1)
            if not raw:
                break
            rows += 1
            if rows > MAX_TRACK_ROWS or stream.tell() > MAX_TRACK_BYTES:
                raise ValueError("Track CSV exceeds the history display scan limit; no partial range is shown")
            if len(raw) > MAX_ROW_BYTES:
                raise ValueError("Oversized AsteRx track row; no partial range is shown")
            if not raw.endswith(b"\n"):
                partial += 1
                break
            value = _point(raw, columns, positions)
            if value is None:
                skipped += 1
                connected = False
                previous_ns = None
                continue
            stamp, point = value
            valid += 1
            lat, lon = point
            south, north = min(south, lat), max(north, lat)
            west, east = min(west, lon), max(east, lon)
            if previous_ns is not None and (stamp < previous_ns or stamp - previous_ns > TRACK_GAP_NS):
                connected = False
            if not connected:
                if len(segments) >= MAX_TRACK_SEGMENTS:
                    raise ValueError("Track has too many disconnected sections for display; no partial range is shown")
                if segments:
                    breaks += 1
                segments.append([])
                connected = True
            points = segments[-1]
            if not points or points[-1] != point:
                points.append(point)
                displayed += 1
                if displayed > MAX_TRACK_POINTS:
                    displayed = _thin(segments)
                    simplified = True
                    if displayed > MAX_TRACK_POINTS:
                        raise ValueError("Track sections exceed the display point budget; no partial range is shown")
            previous_ns = stamp
        if _fingerprint(os.fstat(stream.fileno())) != initial or _fingerprint(source.stat()) != initial:
            raise ValueError("Track CSV changed while reading; retry after recording has stopped")
    _check_cancel(cancel)
    if not valid:
        raise ValueError("No valid timestamped coordinates in this recording")
    return SessionTrack(source, tuple(tuple(points) for points in segments), ((south, west), (north, east)),
                        rows, valid, displayed, skipped, partial, breaks, simplified)
