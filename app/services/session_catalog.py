"""Read-only session browsing from each run manifest; sensor payloads are never read."""

from __future__ import annotations

import asyncio
import json
import os
import re
import threading
from dataclasses import dataclass
from pathlib import Path

from .run_status import HISTORY_SCHEMAS
from ..constants import DRIVERS, SESSION_DIR_RE

_SLOTS = asyncio.Semaphore(2)


@dataclass(frozen=True)
class Session:
    path: Path
    started: str
    drivers: tuple[str, ...]
    status: str
    detail: str
    duration_s: float | None


def _session(path: Path, active: Path | None) -> Session:
    """One session from its manifest; an unreadable manifest is reported, not raised."""
    name = path.name
    try:
        doc = json.loads((path / "raw" / "drivers.json").read_bytes())
        if not isinstance(doc, dict) or doc.get("status_schema") not in HISTORY_SCHEMAS:
            raise ValueError("Unsupported run manifest version")
        run = doc["run"]
        if run["timestamp"] != name:
            raise ValueError("Manifest timestamp does not match the session directory")
        drivers = tuple(k for k in DRIVERS if doc.get("drivers", {}).get(k, {}).get("enabled") is True)
        detail = str(run.get("status", "Unknown recording result"))
        finalized = bool(run.get("ended")) and doc.get("lifecycle", {}).get("phase") == "finished"
        failed = run.get("recording_failed")
        if path == active:
            status = "active"
        elif not finalized or not isinstance(failed, bool):
            status = "unfinished"
        elif failed:
            status = "failed"
        elif detail == "completed":
            status = "completed"
        elif detail.startswith("interrupted (signal "):
            status = "stopped"
        else:
            status = "unknown"
        value = run.get("duration_s")
        duration = float(value) if isinstance(value, (int, float)) and not isinstance(value, bool) else None
        return Session(path, str(run.get("started", name)), drivers, status, detail, duration)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        return Session(path, name, (), "unknown", f"Metadata unavailable: {exc}", None)


def _scan(root: Path, active: Path | None, cancel: threading.Event) -> tuple[list[Session], str]:
    with os.scandir(root) as entries:
        names = sorted((entry.name for entry in entries
                        if re.fullmatch(SESSION_DIR_RE, entry.name) and entry.is_dir(follow_symlinks=False)),
                       reverse=True)
    result = []
    for name in names:
        if cancel.is_set():
            return [], "Cancelled"
        result.append(_session(root / name, active))
    return result, f"{len(result)} sessions"


class Catalog:
    def __init__(self) -> None:
        self.rows: list[Session] = []
        self.note = ""
        self.busy = False
        self.closed = False
        self._cancel = threading.Event()
        self._task = None

    def close(self) -> None:
        self.closed = True
        self._cancel.set()

    async def load(self, root: Path, active: Path | None) -> None:
        if self.busy or self.closed:
            return
        self.busy = True
        self._cancel = threading.Event()

        async def execute():
            try:
                async with _SLOTS:
                    result = await asyncio.to_thread(_scan, root.resolve(), active, self._cancel)
                if not self.closed:
                    self.rows, self.note = result
            except Exception as exc:
                if not self.closed:
                    self.rows, self.note = [], f"Cannot list sessions: {exc}"
            finally:
                self.busy = False

        self._task = asyncio.create_task(execute())
        await asyncio.shield(self._task)
