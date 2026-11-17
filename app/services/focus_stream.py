"""Live focus-assist streams shared by GoX and FX10: launch a snapshot tool in ``--stream-s``
mode, follow its newest records, and stop it with SIGTERM.

The tool keeps acquiring until SIGTERM or its ``--stream-s`` limit, writing short RAW
segments. The caller's ``poll`` reads the newest committed records and deletes the
closed segments behind them (``prune_segments``), so a stream keeps a few segments on
disk however long the focus ring is turned. A stream is a preview: the next stream
replaces its directory, its frames are deleted when it ends, and no configuration is
written.

The stream holds the camera reservation for its whole life (the caller runs it inside
``camera_operations.run``); ``stop_reason`` ends it on Stop or when no Camera Tools page
has polled for HEARTBEAT_TIMEOUT_S (a closed tab cannot keep the camera).
"""

from __future__ import annotations

import asyncio
import collections
import contextlib
import shutil
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from pathlib import Path

import cv2

from ..decoder.ebus import recording as raw_recording
from ..state import STATE
from . import preview_images, snapshot_tool, tool_jobs

MAX_STREAM_S = 1800.0  # --stream-s: bounds a stream whose GUI went away
HEARTBEAT_TIMEOUT_S = 15.0
POLL_S = 0.05
KEEP_SEGMENTS = 3  # newest segments kept on disk while streaming
MAX_READ_ERRORS = 20  # consecutive unreadable newest frames end the stream
OUTPUT_TAIL_BYTES = 256 * 1024
RETIRED_URLS = 6  # replaced images stay servable briefly for a browser still loading them
READ_ERRORS = (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError, cv2.error)


@dataclass(frozen=True)
class StreamResult:
    ok: bool
    reason: str = ""
    raw_output: str = ""


def no_page_reason(last_touch: float) -> str:
    """The heartbeat rule: why to stop when no Camera Tools page polled recently, else ""."""
    if time.monotonic() - last_touch > HEARTBEAT_TIMEOUT_S:
        return "Stopped because no Camera Tools page is open"
    return ""


async def run(argv: list[str], output: Path, *, poll: Callable[[Path], Awaitable[str]],
              stop_reason: Callable[[], str], on_stopping: Callable[[], None]) -> StreamResult:
    """Launch the tool, call ``poll(output)`` until it fails, the tool exits or ``stop_reason()``
    names a reason, then SIGTERM the tool and interpret its exit.

    ``poll`` returns a reason to end the stream, else "". ``on_stopping`` runs once before
    the SIGTERM of a tool that is still running.
    """
    if stop_reason():
        return StreamResult(True, "Stopped before the camera was opened")
    proc = await tool_jobs.spawn(argv)
    stdout, stderr = Tail(), Tail()
    readers = asyncio.gather(stdout.drain(proc.stdout), stderr.drain(proc.stderr))
    exited = asyncio.create_task(proc.wait())
    ended_by, failure = "", ""
    try:
        while not exited.done():
            ended_by = stop_reason()
            if ended_by:
                break
            failure = await poll(output)
            if failure:
                break
            await asyncio.wait({exited}, timeout=POLL_S)
    finally:
        if not exited.done():
            on_stopping()
            await tool_jobs.terminate(proc)  # SIGTERM: the tool stops acquisition and closes its RAW
        try:
            await asyncio.wait_for(readers, 10)
        except (asyncio.TimeoutError, OSError):
            readers.cancel()
        await asyncio.to_thread(drop_segments, output)
    if STATE.tool_operation.get("stop_requested"):
        ended_by = ended_by or "Stopped from the header's Stop tool"
    result = snapshot_tool.interpret(stdout.text(), stderr.text(), proc.returncode)
    if failure:
        return StreamResult(False, failure, result.raw_output)
    if result.ok:
        return StreamResult(True, ended_by or f"Stream reached its {MAX_STREAM_S / 60:g}-minute limit; "
                                              "start it again to continue", result.raw_output)
    # A stop during camera bring-up ends the tool with "no frame" (7) or "interrupted" (130).
    if ended_by and result.reason.startswith(("7 ", "130 ")):
        return StreamResult(True, f"{ended_by} before the first frame", result.raw_output)
    return StreamResult(False, f"Stream failed: {result.reason}", result.raw_output)


class Images:
    """Preview URLs of one stream; a replaced image stays servable for RETIRED_URLS more updates."""

    def __init__(self) -> None:
        self._retired: collections.deque[str] = collections.deque()

    def replace(self, old: str, data: bytes, filename: str) -> str:
        if old:
            self._retired.append(old)
        while len(self._retired) > RETIRED_URLS:
            preview_images.release(self._retired.popleft())
        return preview_images.put(data, filename)

    def release(self, *current: str) -> None:
        preview_images.release(*current, *self._retired)
        self._retired.clear()


class Tail:
    """The last OUTPUT_TAIL_BYTES of a pipe, drained continuously so the tool never blocks on it."""

    def __init__(self) -> None:
        self._chunks: collections.deque[bytes] = collections.deque()
        self._size = 0
        self._truncated = False

    async def drain(self, stream: asyncio.StreamReader) -> None:
        while chunk := await stream.read(65536):
            self._chunks.append(chunk)
            self._size += len(chunk)
            while self._size > OUTPUT_TAIL_BYTES and len(self._chunks) > 1:
                self._size -= len(self._chunks.popleft())
                self._truncated = True

    def text(self) -> str:
        prefix = "[earlier output omitted]\n" if self._truncated else ""
        return prefix + b"".join(self._chunks).decode(errors="replace")


# --- worker-thread helpers (no shared state) ---------------------------------

def fresh_output(root: Path) -> Path:
    """A new stream directory under ``root``; earlier streams there are removed."""
    root.mkdir(parents=True, exist_ok=True)
    for stale in root.iterdir():
        if stale.is_dir():
            shutil.rmtree(stale, ignore_errors=True)
    return root / time.strftime("%Y%m%d_%H%M%S")


def camera_directory(output: Path) -> Path | None:
    """The tool's session directory under ``output``; it exists once the recorder opened."""
    if not output.is_dir():
        return None
    return next((path for path in sorted(output.iterdir()) if path.is_dir() and raw_recording.segment_paths(path)),
                None)


def prune_segments(camera_dir: Path) -> None:
    """Only the writer's newest segment is open; older ones are closed for good."""
    for path in raw_recording.segment_paths(camera_dir)[:-KEEP_SEGMENTS]:
        with contextlib.suppress(FileNotFoundError):
            path.unlink()


def drop_segments(output: Path) -> None:
    """Keep device.json and events.jsonl of the last stream for diagnostics, not its frames."""
    if output.is_dir():
        for path in output.glob("*/seg_*.raw"):
            with contextlib.suppress(FileNotFoundError):
                path.unlink()
