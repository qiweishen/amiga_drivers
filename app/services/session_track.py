"""Schedule bounded, cancellable static track reads for recording history."""

from __future__ import annotations

import asyncio
import threading
from pathlib import Path

from ..decoder.asterx.track import SessionTrack, TrackCancelled, read_track

_SLOTS = asyncio.Semaphore(2)


class TrackView:
    def __init__(self) -> None:
        self.result: SessionTrack | None = None
        self.session: Path | None = None
        self.error = ""
        self.busy = False
        self.closed = False
        self.generation = 0
        self._cancel = threading.Event()

    def close(self) -> None:
        self.closed = True
        self.generation += 1
        self._cancel.set()

    async def load(self, session: Path) -> bool:
        """Return True only if this request remains the page's current selection."""
        if self.closed:
            return False
        self._cancel.set()
        self._cancel = cancel = threading.Event()
        self.generation += 1
        generation = self.generation
        self.session, self.result, self.error, self.busy = session, None, "", True
        try:
            async with _SLOTS:
                result = await asyncio.to_thread(read_track, session, cancel)
            if self.closed or generation != self.generation:
                return False
            self.result = result
            return True
        except TrackCancelled:
            return False
        except asyncio.CancelledError:
            cancel.set()
            raise
        except (OSError, ValueError) as error:
            if self.closed or generation != self.generation:
                return False
            self.error = str(error)
            return True
        finally:
            if generation == self.generation:
                self.busy = False
