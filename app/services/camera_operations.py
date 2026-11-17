"""Event-loop-owned reservations shared by recording and camera tools.

GigE control is exclusive: while a recording runs, a camera tool may touch only a
camera whose driver was disabled at that recording's start ("discovery" is a
broadcast and always allowed; ``None`` means the whole rig must be idle).
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
from typing import TypeVar

from . import control_owner, tool_jobs
from ..state import STATE, ProcState

T = TypeVar("T")
_task: asyncio.Task | None = None
DRIVER_OF_KIND = {"GoX": "gox", "FX10": "fx10"}  # discovery table kind -> driver


class CameraBusyError(RuntimeError):
    pass


def guard_reason(driver: str | None) -> str | None:
    """Why a tool for ``driver`` must not run now, or None. Uses the enables captured
    at recording start: a config edit mid-run must not unlock the camera the driver owns."""
    if STATE.tool_uncertain:
        return "A previous device tool may still be running; waiting for verified exit"
    if STATE.control_uncertain:
        return "Acquisition ownership is unknown; camera tools are locked until it can be verified"
    if STATE.process_state in (ProcState.STARTING, ProcState.STOPPING):
        return "Recording is starting or stopping; wait for the operation to finish"
    if STATE.process_state is ProcState.RUNNING and driver != "discovery":
        if not STATE.ownership_verified or driver is None or STATE.enables_at_start.get(driver, True):
            return "The recording owns this camera; stop recording first"
    return None


async def run(driver: str | None, action: Callable[[], Awaitable[T]], *, kind: str = "camera-tool") -> T:
    """Check and reserve without an await; a detached page cannot release the tool early."""
    global _task
    control_owner.require()
    reason = guard_reason(driver)
    if reason or STATE.snapshot_busy:
        raise CameraBusyError(reason or "Another camera tool is still running")
    STATE.snapshot_busy = True

    async def execute() -> T:
        error = ""
        try:
            await tool_jobs.check_idle()
            tool_jobs.begin(kind, driver)
            result = await action()
            if getattr(result, "ok", True) is False:
                error = getattr(result, "reason", "") or getattr(result, "message", "") or "Tool failed"
            error = error or getattr(result, "error", "")
            return result
        except BaseException as exc:
            error = str(exc) or "Operation interrupted"
            raise
        finally:
            try:
                await tool_jobs.finish(error)
            finally:
                STATE.snapshot_busy = False

    _task = asyncio.create_task(execute())
    # Keep the operation (and reservation) alive if its page stops awaiting it.
    # Retrieving the exception also covers a caller that disconnected.
    _task.add_done_callback(lambda task: None if task.cancelled() else task.exception())
    return await asyncio.shield(_task)
