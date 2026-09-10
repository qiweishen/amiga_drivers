"""Recover a session from the acquisition's open log and recorded metadata."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import yaml

from ..constants import DRIVERS
from . import runtime, run_status

_LOG = re.compile(r"/(\d{8}_\d{6})/raw/log_\1\.log$")


@dataclass(frozen=True)
class SessionInfo:
    path: Path
    enables: dict[str, bool]
    lms_names: list[str]
    started_at: float


async def recover(ref: runtime.ProcessIdentity) -> SessionInfo:
    session = run_status.control_session(ref)
    if session is not None:
        info = read(session)
        if not await runtime.is_process_alive(ref):
            raise ValueError("Acquisition exited while its status was being read")
        return info
    candidates = {
        runtime.to_host_path(path).parent.parent
        for path in await runtime.process_files(ref)
        if _LOG.search(path)
    }
    if len(candidates) != 1:
        raise ValueError("The process does not identify exactly one acquisition session log")
    session = candidates.pop()
    info = read(session)
    if not await runtime.is_process_alive(ref):
        raise ValueError("The acquisition exited while its session was being identified")
    return info


def read(session: Path) -> SessionInfo:
    doc = run_status.read_json(session / "raw" / "drivers.json")
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
        config = session / "raw" / "config" / f"config-lms4xxx_{session.name}.yaml"
        lidar_doc = yaml.safe_load(config.read_text(encoding="utf-8"))
        names = [str(entry["id"]) for entry in lidar_doc["lidar"] if entry.get("enabled", True)]
        if not names or len(set(names)) != len(names):
            raise ValueError("Cannot identify enabled LiDAR instances from the recorded config")
    started = datetime.fromisoformat(run["started"].replace("Z", "+00:00"))
    if started.tzinfo is None:
        raise ValueError("The recorded start time has no timezone")
    return SessionInfo(session, enables, names, started.timestamp())


def finished_cleanly(session: Path) -> bool:
    """Missing or unfinalized metadata cannot certify recording integrity."""
    run = json.loads((session / "raw" / "drivers.json").read_text(encoding="utf-8"))["run"]
    status = str(run.get("status", ""))
    return (run.get("recording_failed") is False and bool(run.get("ended"))
            and (status == "completed" or status.startswith("interrupted (signal ")))
