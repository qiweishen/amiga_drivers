"""Per-sensor live data size + write rate + output-disk usage."""

from __future__ import annotations

import asyncio
import os
import re
import shutil
import time
from pathlib import Path

from ..state import STATE
from . import config_store

# scan_<InstanceName>_<YYYYMMDD_HHMMSS>[_NNN].bin — the name may contain
# spaces/underscores; the timestamp anchors the greedy group.
_LMS_FILE_RE = re.compile(r"^scan_(.+)_\d{8}_\d{6}(?:_\d+)?\.bin$")

_prev: dict[str, tuple[float, int]] = {}  # key -> (t, bytes)


def _du(path: Path) -> int:
    total = 0
    try:
        with os.scandir(path) as it:
            for entry in it:
                try:
                    if entry.is_dir(follow_symlinks=False):
                        total += _du(Path(entry.path))
                    else:
                        total += entry.stat(follow_symlinks=False).st_size
                except OSError:
                    pass
    except OSError:
        pass
    return total


def _collect(session: Path) -> dict[str, int]:
    """Sensor-key -> bytes for the active session's bin/ tree."""
    sizes: dict[str, int] = {}
    bin_dir = session / "bin"
    for driver in ("ins401", "gox", "asterx"):
        d = bin_dir / driver
        if d.is_dir():
            sizes[driver] = _du(d)
    lms_dir = bin_dir / "lms4xxx"
    if lms_dir.is_dir():
        try:
            for entry in os.scandir(lms_dir):
                if not entry.is_file():
                    continue
                m = _LMS_FILE_RE.match(entry.name)
                key = f"lms:{m.group(1)}" if m else "lms:?"
                try:
                    sizes[key] = sizes.get(key, 0) + entry.stat().st_size
                except OSError:
                    pass
        except OSError:
            pass
    return sizes


def _poll_sync() -> None:
    settings = config_store.main_settings()
    output_dir: Path | None = settings["output_dir"]
    if output_dir is not None and output_dir.is_dir():
        usage = shutil.disk_usage(output_dir)
        STATE.storage.disk_total = usage.total
        STATE.storage.disk_used = usage.used
        STATE.storage.disk_free = usage.free
    session = STATE.active_session
    if session is None or not session.is_dir():
        return
    now = time.monotonic()
    sizes = _collect(session)
    for key, st in STATE.sensors.items():
        total = sizes.get(key, 0)
        prev = _prev.get(key)
        if prev is not None and now > prev[0]:
            st.bytes_per_s = max(0.0, (total - prev[1]) / (now - prev[0]))
        st.bytes_total = total
        _prev[key] = (now, total)


async def poll() -> None:
    await asyncio.to_thread(_poll_sync)


def human_bytes(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(n) < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
    return f"{n:.1f} TiB"
