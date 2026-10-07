"""Read and validate the C++ amiga-run-v1 protocol without changing UI state."""

from __future__ import annotations

import json
from pathlib import Path

from ..constants import RUNTIME_DIR
from ..state import SensorState, SensorStatus
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
    return (isinstance(identity, dict) and identity.get("pid") == ref.pid and identity.get("start_ticks") == ref.start_ticks
            and identity.get("boot_id") == ref.boot_id)


def control_session(ref: runtime.ProcessIdentity) -> Path | None:
    path = RUNTIME_DIR / "acquisition.json"
    if not path.exists():
        return None
    doc = read_json(path)
    if not _matches(doc, ref):
        return None  # The previous run's rendezvous cannot identify this process.
    if doc.get("status_schema") != SCHEMA:
        raise ValueError("Unsupported acquisition status protocol")
    return runtime.to_host_path(doc["run"]["session_directory"])


def read_session(session: Path, ref: runtime.ProcessIdentity | None) -> dict:
    doc = read_json(session / "raw" / "drivers.json")
    if ref is None or doc.get("status_schema") != SCHEMA or not _matches(doc, ref):
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


def sensor_statuses(enables: dict[str, bool], lms_names: list[str]) -> dict[str, SensorStatus]:
    sensors = {f"lms:{name}": SensorStatus(f"lms:{name}", f"LMS4xxx · {name}") for name in lms_names or ["?"]}
    for key, label in (("gox", "GoX Cameras"), ("asterx", "AsteRx"), ("fx10", "FX10 Hyperspectral")):
        sensors[key] = SensorStatus(key, label)
    for key, sensor in sensors.items():
        driver = "lms4xxx" if key.startswith("lms:") else key
        sensor.state = SensorState.WAITING if enables.get(driver, False) else SensorState.DISABLED
    return sensors


def sensor_update(doc: dict, sensors: dict[str, SensorStatus]) -> dict[str, tuple[SensorState, str]]:
    lifecycle = doc["lifecycle"]
    states = lifecycle["sensors"]
    # Validate the entire update before exposing any part of it.
    enabled = {key for key, sensor in sensors.items() if sensor.state is not SensorState.DISABLED}
    if set(states) != enabled:
        raise ValueError("Lifecycle sensor identities disagree with the recorded enabled devices")
    parsed = {}
    for key, value in states.items():
        state = SensorState(value["state"])
        if state is SensorState.DISABLED:
            raise ValueError("An enabled sensor cannot become disabled within a run")
        parsed[key] = state, str(value.get("error", ""))
    return parsed
