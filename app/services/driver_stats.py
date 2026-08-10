"""Live per-sensor disk write rate, parsed from the drivers' [Statistics] lines.

CONTRACT with the C++ side (no compiler catches a rename — tools/check_contracts.py
does): every frame-based driver prints its disk write rate as ``fps=<float>`` in
the periodic ``[Statistics]`` line, meaning FRAMES ACTUALLY WRITTEN TO DISK per
second over that driver's last stats window:

    gox_driver/src/stats.cpp                    [Statistics] [cam0] up=… rate=… Hz  fps=24.0  …
    fx10_driver/src/fx10_driver_app.cpp         [Statistics] frames=… rate=… Hz  fps=24.0  …
    lms4xxx_driver/src/lms4xxx_driver_app.cpp   [Statistics] [Front_Right_Laser] up=… rate=… Hz  fps=600.0  …

`rate=` is the sensor's own output rate; `fps=` is what survived to disk, so a
gap between them is exactly the loss the card should surface.

AsteRx records a byte stream rather than frames and prints no fps field — its
card simply has no frame rate.

The GoX driver reports per camera while the dashboard has one GoX card, so the
cameras are summed (breakdown available for the tooltip). Values go stale when
their driver stops printing: the threshold adapts to the observed line period,
because stats_interval_s is configurable per driver.
"""

from __future__ import annotations

import re
import time

from ..state import STATE

# "[Statistics] [cam0] up=…" -> instance token; the plain fx10 line has none.
_INSTANCE_RE = re.compile(r"^\[Statistics\] \[([^\]]+)\] ")
_FPS_RE = re.compile(r"(?:^|\s)fps=(\d+(?:\.\d+)?)")

# Module token -> driver. Statistics lines are emitted from the app layer for
# fx10/lms (App token) but from capture_runner.cpp for gox (internal token), so
# both spellings are accepted for each driver.
_MODULE_TO_DRIVER = {
    "GoX": "gox", "GoXApp": "gox",
    "FX10": "fx10", "FX10App": "fx10",
    "LMS4xxx": "lms", "LMS4xxxApp": "lms",
}

# Floor for the staleness window: 3x the slowest shipped stats period (2.5 s).
_MIN_STALE_AFTER_S = 8.0


class DriverStats:
    """Feeds SensorStatus.write_fps from the log stream (a TAILER subscriber)."""

    def __init__(self) -> None:
        # camera id -> (fps, monotonic) for the summed GoX card
        self._gox_cams: dict[str, tuple[float, float]] = {}
        # sensor key -> observed seconds between [Statistics] lines
        self._period: dict[str, float] = {}

    def reset(self) -> None:
        """Called with MONITOR.reset() on every start/reattach."""
        self._gox_cams.clear()
        self._period.clear()

    # -- ingest --------------------------------------------------------------

    def on_line(self, line) -> None:  # LogLine (duck-typed to keep imports light)
        msg = line.msg
        if not msg.startswith("[Statistics] "):
            return
        driver = _MODULE_TO_DRIVER.get(line.module)
        if driver is None:
            return
        m = _FPS_RE.search(msg)
        if m is None:
            return  # "Final:" summaries and pre-fps binaries carry no rate
        fps = float(m.group(1))
        now = time.monotonic()
        inst = _INSTANCE_RE.match(msg)

        if driver == "gox":
            cam = inst.group(1) if inst else "?"
            prev = self._gox_cams.get(cam)
            if prev is not None and now > prev[1]:
                # capture_runner prints every camera back-to-back in one round,
                # so the summed key only ever sees the microsecond gap BETWEEN
                # cameras. A single camera's own lines are exactly one round
                # apart — that is the period the staleness window needs.
                self._period["gox"] = now - prev[1]
            self._gox_cams[cam] = (fps, now)
            self._publish("gox", self._gox_total(now), now, track_period=False)
        elif driver == "fx10":
            self._publish("fx10", fps, now)
        elif inst is not None:
            self._publish(f"lms:{inst.group(1)}", fps, now)

    def _gox_total(self, now: float) -> float:
        """Sum of the cameras still reporting (a stopped camera must not keep
        inflating the total once the others carry on)."""
        cutoff = now - self._stale_after("gox")
        return sum(fps for fps, at in self._gox_cams.values() if at >= cutoff)

    def _publish(self, key: str, fps: float, now: float, *, track_period: bool = True) -> None:
        st = STATE.sensors.get(key)
        if st is None:
            return  # sensor not in the current enable set (or unknown instance)
        if track_period and st.write_fps_at > 0.0 and now > st.write_fps_at:
            self._period[key] = now - st.write_fps_at
        st.write_fps = fps
        st.write_fps_at = now

    # -- query ---------------------------------------------------------------

    def _stale_after(self, key: str) -> float:
        return max(_MIN_STALE_AFTER_S, 3.0 * self._period.get(key, 0.0))

    def write_fps(self, key: str) -> float | None:
        """Live frames-per-second on disk, or None when the sensor never
        reported one or has gone quiet."""
        st = STATE.sensors.get(key)
        if st is None or st.write_fps_at <= 0.0:
            return None
        if time.monotonic() - st.write_fps_at > self._stale_after(key):
            return None
        return st.write_fps

    def gox_breakdown(self) -> list[tuple[str, float]]:
        """[(camera id, fps)] of the cameras still reporting, for the tooltip."""
        cutoff = time.monotonic() - self._stale_after("gox")
        return sorted((cam, fps) for cam, (fps, at) in self._gox_cams.items() if at >= cutoff)


STATS = DriverStats()
