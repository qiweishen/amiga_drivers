"""Per-sensor live data size + write rate + output-disk usage."""

from __future__ import annotations

import asyncio
import os
import re
import shutil
import time
from dataclasses import dataclass
from pathlib import Path

from ..state import STATE, ProcState, StorageStatus
from . import config_store

# scan_<InstanceName>_<YYYYMMDD_HHMMSS>_NNN.h5 (HDF5 split files) — the name
# may contain underscores; the timestamp anchors the greedy group.
_LMS_FILE_RE = re.compile(r"^scan_(.+)_\d{8}_\d{6}(?:_\d+)?\.h5$")

_prev: dict[str, tuple[float, int]] = {}  # key -> (t, bytes)
_prev_session: tuple | None = None


def _du(path: Path) -> int:
    total = 0
    with os.scandir(path) as it:
        for entry in it:
            if entry.is_dir(follow_symlinks=False):
                total += _du(Path(entry.path))
            else:
                total += entry.stat(follow_symlinks=False).st_size
    return total


def _collect(session: Path) -> dict[str, int]:
    """Sensor-key -> bytes for the active session's raw/ tree."""
    sizes: dict[str, int] = {}
    bin_dir = session / "raw"
    for driver in ("gox", "asterx", "fx10"):
        d = bin_dir / driver
        if d.is_dir():
            sizes[driver] = _du(d)
    lms_dir = bin_dir / "lms4xxx"
    if lms_dir.is_dir():
        with os.scandir(lms_dir) as entries:
            for entry in entries:
                if not entry.is_file():
                    continue
                m = _LMS_FILE_RE.match(entry.name)
                key = f"lms:{m.group(1)}" if m else "lms:?"
                sizes[key] = sizes.get(key, 0) + entry.stat().st_size
    return sizes


@dataclass(frozen=True)
class _Request:
    generation: int
    session: Path | None
    disk_path: Path | None
    disk_scope: str
    next_output: Path | None
    config_error: str
    sensor_keys: tuple[str, ...]


def _request() -> _Request:
    error = ""
    try:
        output = config_store.main_settings()["output_dir"]
        if output is None:
            error = "Next output directory is outside the GUI's shared mounts"
    except Exception as e:
        output = None
        error = f"Cannot read next output directory: {e}"
    active = STATE.process_state in (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING)
    session = STATE.active_session
    return _Request(STATE.session_generation, session,
                    session if active and session is not None else output,
                    "Current session disk" if active and session is not None else "Next recording disk",
                    output, error, tuple(STATE.sensors))


def _measure(request: _Request) -> tuple[StorageStatus, dict[str, int] | None]:
    """Worker-local values only. The event loop decides whether to publish."""
    status = StorageStatus(disk_path=request.disk_path, disk_scope=request.disk_scope,
                           next_output_path=request.next_output, generation=request.generation)
    errors = [request.config_error] if request.config_error else []
    try:
        if request.disk_path is None or not request.disk_path.is_dir():
            raise OSError("Output directory is unavailable")
        usage = shutil.disk_usage(request.disk_path)
        status.disk_total, status.disk_used, status.disk_free = usage.total, usage.used, usage.free
    except OSError as e:
        errors.append(f"Disk info unavailable: {e}")
    sizes = None
    if request.session is not None:
        try:
            if not request.session.is_dir():
                raise OSError("Session directory is unavailable")
            sizes = _collect(request.session)
        except OSError as e:
            errors.append(f"Session sizes were not refreshed: {e}")
    status.sampled_at = time.monotonic()
    status.error = " · ".join(errors)
    return status, sizes


# Self-throttle: a scan costing T seconds waits another 3*T, so the
# recursive du can never consume more than ~25% of a worker thread no matter how
# fast the timer ticks. Matters on the field rig, where the output directory can
# be a slow shared mount holding thousands of segment files.
_MAX_DUTY_CYCLE = 0.25
_next_poll_at = 0.0
_last_request: _Request | None = None
_scan_task: asyncio.Task | None = None


async def poll() -> None:
    global _scan_task
    if _scan_task is not None and not _scan_task.done():
        return
    request = _request()
    if request == _last_request and time.monotonic() < _next_poll_at:
        return
    if request != _last_request:
        STATE.storage = StorageStatus(disk_path=request.disk_path, disk_scope=request.disk_scope,
                                      next_output_path=request.next_output, generation=request.generation)
    _scan_task = asyncio.create_task(_scan(request))
    _scan_task.add_done_callback(lambda task: None if task.cancelled() else task.exception())
    # A cancelled timer must not free the slot while its worker is still reading.
    await asyncio.shield(_scan_task)


async def _scan(request: _Request) -> None:
    global _next_poll_at, _last_request, _prev_session
    started = time.monotonic()
    status, sizes = await asyncio.to_thread(_measure, request)
    if request != _request():
        return
    STATE.storage = status
    session_key = (request.generation, request.session)
    if session_key != _prev_session:
        _prev.clear()
        _prev_session = session_key
    for key, sensor in STATE.sensors.items():
        sensor.bytes_per_s = 0.0
        if sizes is None:
            _prev.pop(key, None)
            continue
        total = sizes.get(key, 0)
        previous = _prev.get(key)
        if previous is not None and status.sampled_at > previous[0] and total >= previous[1]:
            sensor.bytes_per_s = (total - previous[1]) / (status.sampled_at - previous[0])
        sensor.bytes_total = total
        _prev[key] = (status.sampled_at, total)
    _last_request = request
    _next_poll_at = time.monotonic() + (time.monotonic() - started) * (1 / _MAX_DUTY_CYCLE - 1)


def human_bytes(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(n) < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
    return f"{n:.1f} TiB"
