"""Recover only the session identified by the current acquisition manifest."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from ..constants import DRIVERS
from . import runtime, run_status


@dataclass(frozen=True)
class SessionInfo:
    path: Path
    enables: dict[str, bool]
    lms_names: list[str]
    started_at: float


async def recover(ref: runtime.ProcessIdentity) -> SessionInfo:
    session = run_status.control_session(ref)
    if session is None:
        raise ValueError("Waiting for an amiga-run-v1 manifest matching this process")
    info = read(session, ref)
    if not await runtime.is_process_alive(ref):
        raise ValueError("The acquisition exited while its session was being identified")
    return info


def read(session: Path, ref: runtime.ProcessIdentity) -> SessionInfo:
    doc = run_status.read_session(session, ref)
    run = doc["run"]
    output_dir = runtime.to_host_path(run["output_directory"])
    if run["timestamp"] != session.name or output_dir.resolve() != session.parent.resolve():
        raise ValueError("The run manifest does not match the process's session directory")
    drivers = doc["drivers"]
    if not isinstance(drivers, dict) or set(drivers) - set(DRIVERS):
        raise ValueError("Unrecognized driver manifest")
    enables = {driver: False for driver in DRIVERS}
    for driver, entry in drivers.items():
        if not isinstance(entry, dict) or type(entry.get("enabled")) is not bool:
            raise ValueError("Missing recorded driver enable state")
        enables[driver] = entry["enabled"]
    if not any(enables.values()):
        raise ValueError("The run manifest contains no enabled driver")
    names: list[str] = []
    if enables["lms4xxx"]:
        names = [key.removeprefix("lms:") for key in doc["lifecycle"]["sensors"] if key.startswith("lms:")]
        if not names or any(not name for name in names):
            raise ValueError("Cannot identify enabled LiDAR instances from the lifecycle manifest")
    run_status.sensor_update(doc, run_status.sensor_statuses(enables, names))
    started = datetime.fromisoformat(run["started"].replace("Z", "+00:00"))
    if started.tzinfo is None:
        raise ValueError("The recorded start time has no timezone")
    return SessionInfo(session, enables, names, started.timestamp())


def finished_cleanly(session: Path, ref: runtime.ProcessIdentity | None) -> bool:
    """Missing or unfinalized metadata cannot certify recording integrity."""
    doc = run_status.read_session(session, ref)
    run = doc["run"]
    status = str(run.get("status", ""))
    return (doc["lifecycle"]["phase"] == "finished" and run.get("recording_failed") is False and bool(run.get("ended"))
            and (status == "completed" or status.startswith("interrupted (signal ")))
