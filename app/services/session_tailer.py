"""Bounded session-log tail for diagnostics and write-rate statistics.

Lifecycle state comes exclusively from the amiga-run-v1 manifest.
"""

from __future__ import annotations

import asyncio
from collections import deque
from pathlib import Path
from typing import Callable

from ..constants import LOG_POLL_S
from .log_buffer import LogLine, parse_line

REPLAY_TAIL_LINES = 500


class SessionTailer:
    def __init__(self) -> None:
        self._task: asyncio.Task | None = None
        self.ready = False
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
        self.ready = False
        if self._task is not None:
            self._task.cancel()
            self._task = None

    def start(self, log_path: Path, *, replay: bool) -> None:
        """Replay a bounded diagnostic tail, then follow newly appended lines."""
        self.stop()
        self._task = asyncio.get_running_loop().create_task(self._run(log_path, replay))

    async def _run(self, log_path: Path, replay: bool) -> None:
        # The session dir appears before the log file — wait for the file.
        while not log_path.exists():
            await asyncio.sleep(LOG_POLL_S)

        offset = 0
        if replay:
            tail, offset = await asyncio.to_thread(self._scan_existing, log_path)
            for raw in tail:
                self._emit(parse_line(raw))

        buf = b""
        while True:
            try:
                with open(log_path, "rb") as f:
                    f.seek(offset)
                    chunk = f.read(256 * 1024)
                self.ready = True
            except OSError:
                self.ready = False
                await asyncio.sleep(1.0)
                continue
            if chunk:
                offset += len(chunk)
                buf += chunk
                *lines, buf = buf.split(b"\n")
                for line in lines:
                    self._emit(parse_line(line.decode(errors="replace")))
            # Reads only the bytes appended since the last pass (cost is O(new
            # data), not O(file)), so a short period is cheap even on a slow
            # output mount.
            await asyncio.sleep(LOG_POLL_S)

    @staticmethod
    def _scan_existing(log_path: Path) -> tuple[list[str], int]:
        """Read at most the last 512 KiB and retain at most 500 complete lines."""
        tail: deque[str] = deque(maxlen=REPLAY_TAIL_LINES)
        offset = 0
        with open(log_path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            if size > 512 * 1024:
                f.seek(size - 512 * 1024)
                f.readline()  # discard the partial first line
            else:
                f.seek(0)
            offset = f.tell()
            for bline in f:
                if not bline.endswith(b"\n"):
                    # Partial trailing line still being written: leave offset
                    # before it so the follow loop re-reads and assembles it.
                    break
                offset += len(bline)
                raw = bline.decode(errors="replace").rstrip("\n")
                tail.append(raw)
        return list(tail), offset


TAILER = SessionTailer()
