"""Tail the active session's log file — the ONE primary feed for health + logs.

The file log is trace-level, ANSI-free and survives GUI restarts (the attached
stderr pipe covers only pre-session errors). Reattach replay is bounded: the
whole file is scanned in a worker thread for MARKER lines only (driver
lifecycle transitions), while the LogBuffer receives just the last N lines.
"""

from __future__ import annotations

import asyncio
from collections import deque
from pathlib import Path
from typing import Callable

from .log_buffer import LogLine, parse_line
from .markers import REPLAY_MARKER_SUBSTRINGS

# Substrings that make a line load-bearing for HealthMonitor during replay
# (defined in markers.py, the C++ contract mirror).
MARKER_SUBSTRINGS = REPLAY_MARKER_SUBSTRINGS

REPLAY_TAIL_LINES = 500


def is_marker(raw: str) -> bool:
    return any(s in raw for s in MARKER_SUBSTRINGS)


class SessionTailer:
    def __init__(self) -> None:
        self._task: asyncio.Task | None = None
        self._subscribers: list[Callable[[LogLine], None]] = []

    def subscribe(self, cb: Callable[[LogLine], None]) -> None:
        self._subscribers.append(cb)

    def _emit(self, line: LogLine) -> None:
        for cb in self._subscribers:
            try:
                cb(line)
            except Exception:
                pass

    def stop(self) -> None:
        if self._task is not None:
            self._task.cancel()
            self._task = None

    def start(self, log_path: Path, *, replay: bool) -> None:
        """(Re)start tailing. `replay=True` rebuilds state from existing content
        (bounded), then follows; `replay=False` follows from the start of the
        file (fresh session — the file is empty or tiny)."""
        self.stop()
        self._task = asyncio.get_running_loop().create_task(self._run(log_path, replay))

    async def _run(self, log_path: Path, replay: bool) -> None:
        # The session dir appears before the log file — wait for the file.
        while not log_path.exists():
            await asyncio.sleep(0.5)

        offset = 0
        if replay:
            markers, tail, offset = await asyncio.to_thread(self._scan_existing, log_path)
            tail_raws = set(tail)
            for raw in markers:
                if raw not in tail_raws:  # avoid double-feeding markers inside the tail
                    self._emit(parse_line(raw))
            for raw in tail:
                self._emit(parse_line(raw))

        buf = b""
        while True:
            try:
                with open(log_path, "rb") as f:
                    f.seek(offset)
                    chunk = f.read()
            except OSError:
                await asyncio.sleep(1.0)
                continue
            if chunk:
                offset += len(chunk)
                buf += chunk
                *lines, buf = buf.split(b"\n")
                for line in lines:
                    self._emit(parse_line(line.decode(errors="replace")))
            await asyncio.sleep(0.5)

    @staticmethod
    def _scan_existing(log_path: Path) -> tuple[list[str], list[str], int]:
        """Streaming scan (worker thread): marker lines + last-N tail + EOF offset."""
        markers: list[str] = []
        tail: deque[str] = deque(maxlen=REPLAY_TAIL_LINES)
        offset = 0
        with open(log_path, "rb") as f:
            for bline in f:
                if not bline.endswith(b"\n"):
                    # Partial trailing line still being written: leave offset
                    # before it so the follow loop re-reads and assembles it.
                    break
                offset += len(bline)
                raw = bline.decode(errors="replace").rstrip("\n")
                if is_marker(raw):
                    markers.append(raw)
                tail.append(raw)
        return markers, list(tail), offset


TAILER = SessionTailer()
