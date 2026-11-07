"""Poll AsteRx telemetry and publish decoded data on the event loop."""

from __future__ import annotations

import asyncio
from pathlib import Path

from ..decoder.asterx.recording import CSV_COLUMNS, TailCursor, read_batch
from ..decoder.asterx.telemetry import AsterxTelemetry
from .log_buffer import BUFFER, parse_line

POLL_S = 0.5


class AsterxLive(AsterxTelemetry):
    def __init__(self) -> None:
        super().__init__()
        self._task: asyncio.Task | None = None
        self._gen = 0
        self.session_dir: Path | None = None
        self.error = ""
        self.skipped_rows = 0

    def start(self, session: Path, *, replay: bool) -> None:
        self.stop()
        self.session_dir = session
        self._task = asyncio.create_task(self._run(session / "raw" / "asterx", replay, self._gen))

    def stop(self) -> None:
        self._gen += 1
        if self._task is not None:
            self._task.cancel()
            self._task = None
        self.session_dir = None
        self.error = ""
        self.skipped_rows = 0
        self.reset()

    async def _run(self, directory: Path, replay: bool, gen: int) -> None:
        cursors = {name: TailCursor(tail_on_open=replay) for name in CSV_COLUMNS}
        reported: set[str] = set()
        while gen == self._gen:
            updated, rows, errors, reset, skipped = await asyncio.to_thread(read_batch, directory, cursors)
            if gen != self._gen:
                return
            cursors = updated
            if reset:
                self.reset()
            self.skipped_rows += skipped
            for error in errors:
                if error not in reported:
                    BUFFER.append(parse_line(f"asterx live: {error}", fallback_module="gui"))
                    reported.add(error)
            if errors:
                self.error = " · ".join(errors)
            elif not any(cursor.disabled for cursor in cursors.values()):
                self.error = ""
            if self.skipped_rows:
                self.error = f"{self.error.split(' · Skipped')[0]} · Skipped malformed CSV rows: {self.skipped_rows}".strip(
                    " ·")
            for name, columns, fields in rows:
                self.ingest(name, columns, fields)
            await asyncio.sleep(POLL_S)


LIVE = AsterxLive()
