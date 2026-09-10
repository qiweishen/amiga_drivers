"""Bounded, read-only session metadata browsing. Never traverses sensor payloads."""

from __future__ import annotations

import asyncio
import heapq
import json
import os
import re
import threading
from dataclasses import dataclass
from pathlib import Path

from ..constants import DRIVERS, SESSION_DIR_RE

_SLOTS = asyncio.Semaphore(2)
MAX_SESSIONS = 5000
MAX_ENTRIES = 100000
MAX_MANIFEST = 256 * 1024
MAX_METADATA = 64 * 1024 * 1024


@dataclass(frozen=True)
class Session:
    path: Path
    started: str
    drivers: tuple[str, ...]
    status: str
    detail: str
    duration_s: float | None
    replay: bool


def _scan(root: Path, active: Path | None, cancel: threading.Event) -> tuple[list[Session], str]:
    candidates = []
    seen = matching = 0
    limited = False
    with os.scandir(root) as entries:
        for entry in entries:
            if cancel.is_set():
                return [], "Cancelled"
            seen += 1
            if seen > MAX_ENTRIES:
                limited = True
                break
            if re.fullmatch(SESSION_DIR_RE, entry.name) and entry.is_dir(follow_symlinks=False):
                matching += 1
                heapq.heappush(candidates, entry.name)
                if len(candidates) > MAX_SESSIONS:
                    heapq.heappop(candidates)
    result, metadata_bytes = [], 0
    for name in sorted(candidates, reverse=True):
        if cancel.is_set():
            return [], "Cancelled"
        path = root / name
        status, detail, started, drivers, duration, finalized = "unknown", "", name, (), None, False
        try:
            with (path / "raw" / "drivers.json").open("rb") as stream:
                data = stream.read(MAX_MANIFEST + 1)
            metadata_bytes += len(data)
            if metadata_bytes > MAX_METADATA:
                limited = True
                break
            if len(data) > MAX_MANIFEST:
                raise ValueError("Manifest exceeds the metadata preview limit")
            doc = json.loads(data)
            run = doc["run"]
            if run["timestamp"] != name:
                raise ValueError("Manifest timestamp does not match the session directory")
            started = str(run.get("started", name))
            drivers = tuple(k for k in DRIVERS if doc.get("drivers", {}).get(k, {}).get("enabled") is True)
            detail = str(run.get("status", "Unknown recording result"))
            finalized = bool(run.get("ended"))
            if path == active:
                status = "active"
            elif finalized and run.get("recording_failed") is True:
                status = "failed"
            elif finalized and run.get("recording_failed") is False:
                status = "completed" if detail == "completed" else "stopped" if detail.startswith("interrupted (signal ") else "unknown"
            else:
                status = "unfinished"
            value = run.get("duration_s")
            if isinstance(value, (int, float)) and not isinstance(value, bool) and 0 <= value < 1e10:
                duration = float(value)
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
            detail = f"Metadata unavailable: {exc}"
        csv_available = any((path / "raw" / "asterx" / filename).is_file()
                            for filename in ("live_insnavgeod.csv", "live_receiverstatus.csv"))
        result.append(Session(path, started, drivers, status, detail, duration,
                              finalized and path != active and csv_available))
    note = f"{len(result)} sessions indexed; only top-level directories and small manifests were read"
    if limited or matching > MAX_SESSIONS:
        note += f". Partial listing: limit {MAX_SESSIONS} sessions / {MAX_ENTRIES} entries / 64 MiB metadata"
    return result, note


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
