"""Single UI-facing application state (bindable plain objects, no NiceGUI)."""

from __future__ import annotations

import enum
from collections.abc import Callable
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
    WAITING = "waiting"  # process started, initialization not yet confirmed by manifest
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
    disk_path: Path | None = None
    disk_scope: str = "Next recording disk"
    next_output_path: Path | None = None
    generation: int = 0
    sampled_at: float = 0.0
    error: str = ""


@dataclass(frozen=True)
class RecordingSnapshot:
    """Value returned by the acquisition owner; UI code cannot change its lifecycle."""
    process_state: ProcState = ProcState.IDLE
    attached: bool = True  # False after reattach (exit code unknowable)
    exit_code: int | None = None
    last_error: str = ""  # last [Main] failure line / launch stderr tail
    active_session: Path | None = None  # <Output Directory>/<timestamp>
    session_started_at: float | None = None  # time.time()
    enables_at_start: dict[str, bool] = field(default_factory=dict)
    status_source: str = "unknown"
    status_detail: str = ""
    run_result: dict = field(default_factory=dict)
    stop_requested: bool = False
    config_locked: bool = False  # held until initialization finishes, including a stop during startup
    ownership_verified: bool = False
    control_uncertain: bool = True  # cleared only after checking the execution environment
    session_generation: int = 0  # invalidates work submitted for an earlier run


_RECORDING_FIELDS = frozenset(RecordingSnapshot.__dataclass_fields__)


def _initial_recording_value(name: str):
    return getattr(RecordingSnapshot(), name)


@dataclass
class AppState:
    """Display/operation state with read-only compatibility access to recording.

    AcquisitionController binds the snapshot reader once. Existing pages may
    keep reading STATE.process_state etc.; only the controller owns those values.
    Storage and log-rate services retain their own presentation responsibilities.
    """
    env_ok: bool = False
    env_detail: str = ""
    pending_config_notice: bool = False
    sensors: dict[str, SensorStatus] = field(default_factory=dict)
    storage: StorageStatus = field(default_factory=StorageStatus)
    snapshot_busy: bool = False
    tool_uncertain: bool = True
    tool_operation: dict = field(default_factory=dict)
    controller_connected: bool = False
    _recording_reader: Callable[[], RecordingSnapshot] = field(default=RecordingSnapshot, repr=False)
    _recording_value: Callable[[str], object] = field(default=_initial_recording_value, repr=False)

    def bind_recording(self, reader: Callable[[], RecordingSnapshot], value_reader: Callable[[str], object]) -> None:
        self._recording_reader = reader
        self._recording_value = value_reader

    @property
    def recording(self) -> RecordingSnapshot:
        return self._recording_reader()

    def __getattr__(self, name: str):
        if name in _RECORDING_FIELDS:
            return self._recording_value(name)
        raise AttributeError(name)

    def __setattr__(self, name: str, value) -> None:
        if name in _RECORDING_FIELDS:
            raise AttributeError(f"{name} is owned by AcquisitionController")
        super().__setattr__(name, value)


STATE = AppState()
