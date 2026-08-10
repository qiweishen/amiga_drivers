"""Single UI-facing application state (bindable plain objects, no NiceGUI)."""

from __future__ import annotations

import enum
from dataclasses import dataclass, field
from pathlib import Path


class ProcState(enum.Enum):
    IDLE = "idle"
    STARTING = "starting"
    RUNNING = "running"
    STOPPING = "stopping"
    EXITED = "exited"
    FAILED = "failed"


class SensorState(enum.Enum):
    DISABLED = "disabled"
    WAITING = "waiting"  # process started, no init marker yet
    RUNNING = "running"
    STOPPED = "stopped"
    FAILED = "failed"


@dataclass
class SensorStatus:
    key: str  # "lms:<InstanceName>" | "gox" | "asterx" | "fx10"
    label: str
    state: SensorState = SensorState.DISABLED
    last_error: str = ""
    bytes_total: int = 0  # current session raw-data size
    bytes_per_s: float = 0.0  # write rate between storage polls
    # Frames actually written to disk per second, straight from the driver's
    # periodic [Statistics] line (services/driver_stats.py). 0.0 with
    # write_fps_at == 0.0 means "never reported"; staleness is judged by the
    # service, not here.
    write_fps: float = 0.0
    write_fps_at: float = 0.0  # time.monotonic() of the last [Statistics] line


@dataclass
class StorageStatus:
    disk_total: int = 0
    disk_used: int = 0
    disk_free: int = 0


@dataclass
class AppState:
    mode: str = "?"  # "docker" | "native" (resolved at startup)
    env_ok: bool = False  # docker: container running; native: always True
    env_detail: str = ""  # human reason when env_ok is False
    process_state: ProcState = ProcState.IDLE
    attached: bool = True  # False after reattach (exit code unknowable)
    exit_code: int | None = None
    last_error: str = ""  # last [Main] failure line / launch stderr tail
    active_session: Path | None = None  # <Output Directory>/<timestamp>
    session_started_at: float | None = None  # time.time()
    enables_at_start: dict[str, bool] = field(default_factory=dict)
    pending_config_notice: bool = False  # saved config awaiting next start
    sensors: dict[str, SensorStatus] = field(default_factory=dict)
    storage: StorageStatus = field(default_factory=StorageStatus)
    snapshot_busy: bool = False


STATE = AppState()
