"""Tail the asterx driver's live_*.csv files into in-memory telemetry.

Data flow: C++ Session::on_sbf_block_ -> <session>/bin/asterx/live_*.csv
(append-only, one file per session, survives reconnects) -> this tailer
(0.5 s poll, offset + partial-line buffer, header-mapped columns) ->
deques/snapshots read by the /asterx page timer.

CSV contract (C++ side): values are already in physical units (deg, m, m/s,
deg/s, degC, s); float do-not-use is written as the literal 'nan'; integer
do-not-use keeps its sentinel (255 / 65535 / 4294967295). No unit conversion
happens here.
"""

from __future__ import annotations

import asyncio
import math
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .log_buffer import BUFFER, parse_line


POLL_S = 0.5
REPLAY_TAIL_BYTES = 256 * 1024  # reattach: only the recent tail matters
IMU_STRIDE = 20  # 200 Hz -> 10 Hz chart feed
IMU_POINTS = 300  # 10 Hz x 300 = 30 s window
TRACK_MAX = 2000  # decimate-by-2 on overflow
TRACK_MIN_STEP_DEG = 5e-6  # ~0.55 m — dedupe stationary points
STALE_S = 3.0  # page greys a snapshot older than this

U8_DNU = 255  # nr_sv sentinel
U32_DNU = 4294967295  # tow_ms sentinel

_IMU_COLS = ("acc_x_mps2", "acc_y_mps2", "acc_z_mps2",
             "gyro_x_degps", "gyro_y_degps", "gyro_z_degps")


@dataclass
class PvtSnapshot:
    tow_ms: int | None  # None = DNU (4294967295)
    mode: int
    error: int
    nr_sv: int | None  # None = DNU (255)
    lat_deg: float
    lon_deg: float
    height_m: float
    h_acc_m: float  # nan = DNU
    v_acc_m: float
    vn_mps: float
    ve_mps: float
    vu_mps: float
    cog_deg: float
    mono: float  # time.monotonic() at ingest, for staleness


@dataclass
class InsSnapshot:
    tow_ms: int | None
    gnss_mode: int
    error: int
    info: int
    gnss_age_s: float  # nan = DNU
    lat_deg: float
    lon_deg: float
    height_m: float
    accuracy_m: float  # nan = DNU
    heading_deg: float  # nan = attitude sub-block absent
    pitch_deg: float
    roll_deg: float
    mono: float


@dataclass
class RxStatusSnapshot:
    tow_ms: int | None
    cpu_load_pct: int | None
    up_time_s: int | None
    rx_status: int
    rx_error: int
    temp_c: float
    mono: float


@dataclass
class AttSnapshot:
    tow_ms: int | None
    nr_sv: int | None  # None = DNU (255)
    error: int
    heading_deg: float  # nan = DNU
    pitch_deg: float
    roll_deg: float
    mono: float


def _f(colmap: dict[str, int], fields: list[str], name: str) -> float:
    """Float column: missing/empty/malformed -> nan ('nan' literals parse as-is)."""
    try:
        return float(fields[colmap[name]])
    except (KeyError, IndexError, ValueError):
        return float("nan")


def _i(colmap: dict[str, int], fields: list[str], name: str, dnu: int | None = None) -> int | None:
    """Integer column: malformed or equal to the sentinel `dnu` -> None."""
    try:
        v = int(float(fields[colmap[name]]))
    except (KeyError, IndexError, ValueError):
        return None
    return None if v == dnu else v


class _FileTail:
    """Cursor over one live CSV: byte offset + partial-line buffer + column map."""

    def __init__(self, name: str, required: tuple[str, ...],
                 ingest: Callable[[dict[str, int], list[str]], None]) -> None:
        self.name = name
        self.required = required
        self.ingest = ingest
        self.offset = 0
        self.buf = b""
        self.colmap: dict[str, int] | None = None
        self.bad_header = False


class AsterxLive:
    """Worker thread writes, event loop reads — atomic reference swaps and
    deque appends only (same lock-free shape as storage._poll_sync)."""

    def __init__(self) -> None:
        self._task: asyncio.Task | None = None
        self.session_dir: Path | None = None
        # --- telemetry (page-facing) ---
        self.imu: deque[tuple[float, ...]] = deque(maxlen=IMU_POINTS)  # (t_s, ax,ay,az,gx,gy,gz)
        self.imu_temp_c: float | None = None
        self.imu_mono: float | None = None  # last ExtSensorMeas row, for staleness
        self.track: list[tuple[float, float]] = []  # [(lat_deg, lon_deg)]
        self.pvt: PvtSnapshot | None = None
        self.ins: InsSnapshot | None = None
        self.rx: RxStatusSnapshot | None = None
        self.att: AttSnapshot | None = None
        self.imu_version = 0
        self.track_version = 0
        self.snap_version = 0
        # --- internal ---
        self._gen = 0  # session generation; stale worker polls become no-ops
        self._stride = 0
        self._last_t = 0.0
        self._ins_seen = False
        self._tails = self._make_tails()

    def start(self, session: Path, *, replay: bool) -> None:
        """(Re)start tailing `<session>/bin/asterx`. `replay=True` (reattach)
        seeks to the recent tail of pre-existing files instead of re-reading
        them; `replay=False` follows a fresh session from the top."""
        self.stop()
        self.session_dir = session
        self._reset_data()
        # cancel() cannot interrupt an in-flight to_thread poll of the OLD
        # session; the generation token makes such a straggler a no-op instead
        # of letting it inject stale rows into the freshly reset telemetry
        self._gen += 1
        self._task = asyncio.get_running_loop().create_task(
            self._run(session / "bin" / "asterx", replay, self._gen))

    def stop(self) -> None:
        if self._task is not None:
            self._task.cancel()
            self._task = None

    async def _run(self, csv_dir: Path, replay: bool, gen: int) -> None:
        # Files appear late (driver init) or never (asterx disabled / block not
        # subscribed) — poll forever, each file independently.
        while True:
            await asyncio.to_thread(self._poll_sync, csv_dir, replay, gen)
            replay = False  # tail-seek applies only to files present on the first pass
            await asyncio.sleep(POLL_S)

    # ------------------------------------------------------------------ intake

    def _poll_sync(self, csv_dir: Path, replay: bool, gen: int) -> None:
        if gen != self._gen:
            return
        for t in self._tails:
            if gen != self._gen:  # a newer session took over mid-poll
                return
            if t.bad_header:
                continue
            path = csv_dir / t.name
            if not path.exists():
                continue
            try:
                with open(path, "rb") as f:
                    if t.colmap is None:  # first sight: map the header, position the cursor
                        header = f.readline()
                        if not header.endswith(b"\n"):
                            continue  # header still being written
                        names = header.decode(errors="replace").rstrip("\n").split(",")
                        colmap = {n: i for i, n in enumerate(names)}
                        missing = [r for r in t.required if r not in colmap]
                        if missing:
                            # Contract drift: warn once, disable this stream only.
                            t.bad_header = True
                            BUFFER.append(parse_line(
                                f"asterx live: {t.name} missing column(s) "
                                f"{', '.join(missing)} — stream disabled",
                                fallback_module="gui"))
                            continue
                        t.colmap = colmap
                        t.offset = len(header)
                        size = path.stat().st_size
                        if replay and size - t.offset > REPLAY_TAIL_BYTES:
                            f.seek(size - REPLAY_TAIL_BYTES)
                            junk = f.readline()  # drop the partial line at the cut
                            t.offset = size - REPLAY_TAIL_BYTES + len(junk)
                    f.seek(t.offset)
                    chunk = f.read()
            except OSError:
                continue
            if not chunk:
                continue
            t.offset += len(chunk)
            t.buf += chunk
            *lines, t.buf = t.buf.split(b"\n")  # keep the partial tail
            for line in lines:
                if not line:
                    continue
                fields = line.decode(errors="replace").split(",")
                if len(fields) < len(t.colmap):
                    continue  # truncated/garbage row (the writer emits full rows)
                try:
                    t.ingest(t.colmap, fields)
                except Exception:
                    pass  # one bad row must never kill the tailer

    def _make_tails(self) -> list[_FileTail]:
        return [
            _FileTail("live_pvtgeodetic.csv",
                      ("tow_ms", "host_unix_ns", "mode", "error", "nr_sv",
                       "lat_deg", "lon_deg", "height_m", "vn_mps", "ve_mps",
                       "vu_mps", "cog_deg", "h_acc_m", "v_acc_m"),
                      self._ingest_pvt),
            _FileTail("live_extsensormeas.csv",
                      ("tow_ms", "host_unix_ns", "acc_x_mps2", "acc_y_mps2",
                       "acc_z_mps2", "gyro_x_degps", "gyro_y_degps", "gyro_z_degps"),
                      self._ingest_imu),
            _FileTail("live_insnavgeod.csv",
                      ("tow_ms", "host_unix_ns", "gnss_mode", "error", "info",
                       "gnss_age_s", "lat_deg", "lon_deg", "height_m",
                       "accuracy_m", "heading_deg", "pitch_deg", "roll_deg"),
                      self._ingest_ins),
            _FileTail("live_receiverstatus.csv",
                      ("tow_ms", "host_unix_ns", "cpu_load_pct", "up_time_s",
                       "rx_status", "rx_error", "temp_c"),
                      self._ingest_rx),
            _FileTail("live_atteuler.csv",
                      ("tow_ms", "host_unix_ns", "nr_sv", "error",
                       "heading_deg", "pitch_deg", "roll_deg"),
                      self._ingest_att),
        ]

    def _reset_data(self) -> None:
        """Session switch: drop telemetry; bump versions (not reset) so page
        dirty-checks notice the now-empty data."""
        self.imu.clear()
        self.imu_temp_c = None
        self.imu_mono = None
        self.track = []
        self.pvt = self.ins = self.rx = self.att = None
        self.imu_version += 1
        self.track_version += 1
        self.snap_version += 1
        self._stride = 0
        self._last_t = 0.0
        self._ins_seen = False
        self._tails = self._make_tails()

    # ------------------------------------------------------------------ ingest

    def _track_append(self, lat: float, lon: float) -> None:
        if not (math.isfinite(lat) and math.isfinite(lon)):
            return
        if self.track:
            plat, plon = self.track[-1]
            if abs(lat - plat) < TRACK_MIN_STEP_DEG and abs(lon - plon) < TRACK_MIN_STEP_DEG:
                return  # stationary — keep the track bounded
        self.track.append((lat, lon))
        if len(self.track) > TRACK_MAX:
            # Decimate the whole history: full-route shape, bounded memory.
            self.track = self.track[::2]
        self.track_version += 1

    def _ingest_pvt(self, colmap: dict[str, int], fields: list[str]) -> None:
        lat = _f(colmap, fields, "lat_deg")
        lon = _f(colmap, fields, "lon_deg")
        if not self._ins_seen:  # single-source track: INS owns it once seen
            self._track_append(lat, lon)
        self.pvt = PvtSnapshot(
            tow_ms=_i(colmap, fields, "tow_ms", dnu=U32_DNU),
            mode=_i(colmap, fields, "mode") or 0,
            error=_i(colmap, fields, "error") or 0,
            nr_sv=_i(colmap, fields, "nr_sv", dnu=U8_DNU),
            lat_deg=lat,
            lon_deg=lon,
            height_m=_f(colmap, fields, "height_m"),
            h_acc_m=_f(colmap, fields, "h_acc_m"),
            v_acc_m=_f(colmap, fields, "v_acc_m"),
            vn_mps=_f(colmap, fields, "vn_mps"),
            ve_mps=_f(colmap, fields, "ve_mps"),
            vu_mps=_f(colmap, fields, "vu_mps"),
            cog_deg=_f(colmap, fields, "cog_deg"),
            mono=time.monotonic(),
        )
        self.snap_version += 1

    def _ingest_imu(self, colmap: dict[str, int], fields: list[str]) -> None:
        self.imu_mono = time.monotonic()
        temp = _f(colmap, fields, "temp_c")
        if math.isfinite(temp):
            self.imu_temp_c = temp
        vals = tuple(_f(colmap, fields, n) for n in _IMU_COLS)
        if not all(map(math.isfinite, vals)):
            return  # acc-only / gyro-only blocks: temperature update only
        self._stride += 1
        if self._stride % IMU_STRIDE:
            return
        tow = _i(colmap, fields, "tow_ms", dnu=U32_DNU)
        # Boot-mode frames carry DNU tow: extrapolate to keep the x axis monotonic.
        t = tow / 1000.0 if tow is not None else self._last_t + 0.005 * IMU_STRIDE
        self._last_t = t
        self.imu.append((t, *vals))
        self.imu_version += 1

    def _ingest_ins(self, colmap: dict[str, int], fields: list[str]) -> None:
        lat = _f(colmap, fields, "lat_deg")
        lon = _f(colmap, fields, "lon_deg")
        if math.isfinite(lat) and math.isfinite(lon):
            self._ins_seen = True  # the track is INS-only from now on (no dual-source jitter)
            self._track_append(lat, lon)
        self.ins = InsSnapshot(
            tow_ms=_i(colmap, fields, "tow_ms", dnu=U32_DNU),
            gnss_mode=_i(colmap, fields, "gnss_mode") or 0,
            error=_i(colmap, fields, "error") or 0,
            info=_i(colmap, fields, "info") or 0,
            gnss_age_s=_f(colmap, fields, "gnss_age_s"),
            lat_deg=lat,
            lon_deg=lon,
            height_m=_f(colmap, fields, "height_m"),
            accuracy_m=_f(colmap, fields, "accuracy_m"),
            heading_deg=_f(colmap, fields, "heading_deg"),
            pitch_deg=_f(colmap, fields, "pitch_deg"),
            roll_deg=_f(colmap, fields, "roll_deg"),
            mono=time.monotonic(),
        )
        self.snap_version += 1

    def _ingest_rx(self, colmap: dict[str, int], fields: list[str]) -> None:
        self.rx = RxStatusSnapshot(
            tow_ms=_i(colmap, fields, "tow_ms", dnu=U32_DNU),
            cpu_load_pct=_i(colmap, fields, "cpu_load_pct"),
            up_time_s=_i(colmap, fields, "up_time_s"),
            rx_status=_i(colmap, fields, "rx_status") or 0,
            rx_error=_i(colmap, fields, "rx_error") or 0,
            temp_c=_f(colmap, fields, "temp_c"),
            mono=time.monotonic(),
        )
        self.snap_version += 1

    def _ingest_att(self, colmap: dict[str, int], fields: list[str]) -> None:
        self.att = AttSnapshot(
            tow_ms=_i(colmap, fields, "tow_ms", dnu=U32_DNU),
            nr_sv=_i(colmap, fields, "nr_sv", dnu=U8_DNU),
            error=_i(colmap, fields, "error") or 0,
            heading_deg=_f(colmap, fields, "heading_deg"),
            pitch_deg=_f(colmap, fields, "pitch_deg"),
            roll_deg=_f(colmap, fields, "roll_deg"),
            mono=time.monotonic(),
        )
        self.snap_version += 1


LIVE = AsterxLive()
