"""Validate and apply the C++ amiga-run-v1 lifecycle protocol."""

from __future__ import annotations

import json
from pathlib import Path

from ..constants import RUNTIME_DIR
from ..state import STATE, ProcState, SensorState
from . import runtime

SCHEMA = "amiga-run-v1"


def read_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        text = stream.read(1024 * 1024 + 1)
    if len(text) > 1024 * 1024:
        raise ValueError("Run manifest exceeds the status size limit")
    doc = json.loads(text)
    if not isinstance(doc, dict):
        raise ValueError("Run manifest must be an object")
    return doc


def _matches(doc: dict, ref: runtime.ProcessIdentity) -> bool:
    identity = doc.get("process", {})
    return (identity.get("pid") == ref.pid and identity.get("start_ticks") == ref.start_ticks
            and identity.get("boot_id") == ref.boot_id)


def control_session(ref: runtime.ProcessIdentity) -> Path | None:
    path = RUNTIME_DIR / "acquisition.json"
    if not path.exists():
        return None
    doc = read_json(path)
    if not _matches(doc, ref):
        return None  # Previous run's rendezvous; legacy recovery may identify this process.
    if doc.get("status_schema") != SCHEMA:
        raise ValueError("Unsupported acquisition status protocol")
    return runtime.to_host_path(doc["run"]["session_directory"])


def read_session(session: Path, ref: runtime.ProcessIdentity) -> dict | None:
    doc = read_json(session / "raw" / "drivers.json")
    if "status_schema" not in doc:
        return None
    if doc["status_schema"] != SCHEMA or not _matches(doc, ref):
        raise ValueError("Status protocol or process identity does not match the active recording")
    run, lifecycle = doc["run"], doc["lifecycle"]
    if (run["timestamp"] != session.name
            or runtime.to_host_path(run["session_directory"]).resolve() != session.resolve()):
        raise ValueError("Status document identifies a different session")
    if lifecycle.get("phase") not in ("initializing", "running", "stopping", "finished"):
        raise ValueError("Unknown acquisition lifecycle phase")
    if type(lifecycle.get("configuration_read")) is not bool or not isinstance(lifecycle.get("sensors"), dict):
        raise ValueError("Incomplete lifecycle status")
    return doc


def apply(doc: dict) -> None:
    lifecycle = doc["lifecycle"]
    states = lifecycle["sensors"]
    # Validate the entire update before exposing any part of it.
    enabled = {key for key, sensor in STATE.sensors.items() if sensor.state is not SensorState.DISABLED}
    if set(states) != enabled:
        raise ValueError("Lifecycle sensor identities disagree with the recorded enabled devices")
    parsed = {}
    for key, value in states.items():
        state = SensorState(value["state"])
        if state is SensorState.DISABLED:
            raise ValueError("An enabled sensor cannot become disabled within a run")
        parsed[key] = state, str(value.get("error", ""))
    STATE.status_source = SCHEMA
    STATE.status_detail = lifecycle["phase"]
    STATE.run_result = {"run": doc["run"], "driver_results": doc.get("driver_results", [])}
    for key, (state, error) in parsed.items():
        STATE.sensors[key].state = state
        STATE.sensors[key].last_error = error
    phase = lifecycle["phase"]
    if phase == "running" and lifecycle["configuration_read"]:
        STATE.config_locked = False
        if not STATE.stop_requested:
            STATE.process_state = ProcState.RUNNING
    elif phase in ("stopping", "finished"):
        STATE.process_state = ProcState.STOPPING
        STATE.config_locked = True
    else:
        STATE.config_locked = True
