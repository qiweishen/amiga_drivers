"""Event-loop-owned reservations shared by recording and camera tools."""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
from typing import TypeVar

from ..state import STATE, ProcState
from . import runtime

T = TypeVar("T")
_task: asyncio.Task | None = None


class CameraBusyError(RuntimeError):
    pass


def guard_reason(driver: str | None) -> str | None:
    if STATE.control_uncertain:
        return "Acquisition ownership is unknown; camera tools are locked until it can be verified"
    if not STATE.env_ok:
        return "The execution environment is unavailable"
    if STATE.process_state in (ProcState.STARTING, ProcState.STOPPING):
        return "Recording is starting or stopping; wait for the operation to finish"
    if STATE.process_state is ProcState.RUNNING:
        if not STATE.ownership_verified or driver is None or STATE.enables_at_start.get(driver, True):
            return "The recording owns this camera; stop recording first"
    return None


async def run(driver: str | None, action: Callable[[], Awaitable[T]]) -> T:
    """Check and reserve without an await; a detached page cannot release the tool early."""
    global _task
    reason = guard_reason(driver)
    if reason or STATE.snapshot_busy:
        raise CameraBusyError(reason or "Another camera tool is still running")
    STATE.snapshot_busy = True

    async def execute() -> T:
        try:
            # A collector may outlive a GUI restart (especially docker exec).
            if await runtime.pgrep("fx10_reference"):
                raise CameraBusyError("A reference collector is still running; wait for it to finish")
            return await action()
        finally:
            STATE.snapshot_busy = False

    _task = asyncio.create_task(execute())
    # Keep the operation (and reservation) alive if its page stops awaiting it.
    # Retrieving the exception also covers a caller that disconnected.
    _task.add_done_callback(lambda task: None if task.cancelled() else task.exception())
    return await asyncio.shield(_task)
