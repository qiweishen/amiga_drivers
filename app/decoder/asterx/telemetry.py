"""AsteRx telemetry samples, display track, and recorded-host-time freshness.

host_unix_ns is the host CSV recorder's clock, not a verified device sampling
time. Reading a row must never make old telemetry appear fresh.
"""

from __future__ import annotations

import math
import time
from collections.abc import Sequence
from dataclasses import dataclass

TRACK_MAX = 2000
TRACK_MIN_STEP_DEG = 5e-6
STALE_S = 3.0
U32_DNU = 4294967295
U8_DNU = 255

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


def sample_age_s(sample: InsSnapshot | RxStatusSnapshot) -> float | None:
    stamp = sample.host_unix_ns
    if stamp is None or stamp <= 0:
        return None
    elapsed = time.monotonic() - sample.mono
    wall_elapsed = (time.time_ns() - sample.read_unix_ns) / 1e9
    # A wall-clock adjustment makes freshness uncertain until another row arrives.
    if abs(elapsed - wall_elapsed) > 1.0 or stamp > sample.read_unix_ns:
        return None
    return (sample.read_unix_ns - stamp) / 1e9 + max(0.0, elapsed)

class AsterxTelemetry:
    """Display model populated by the current acquisition's live tailer."""

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
