"""Start and stop the single-container service assembly."""

from __future__ import annotations

import asyncio

from . import control_owner, process, storage, tool_jobs
from .asterx_live import LIVE as ASTERX_LIVE
from .driver_stats import STATS
from .log_buffer import BUFFER
from .session_tailer import TAILER
from ..constants import RUNTIME_DIR
from ..state import STATE, ProcState

_tasks: list[asyncio.Task] = []
_closing = False
_subscribed = False


async def startup() -> None:
    global _closing, _subscribed
    _closing = False
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    control_owner.acquire()
    if not _subscribed:
        TAILER.subscribe(STATS.on_line)
        TAILER.subscribe(BUFFER.append_session)
        _subscribed = True
    await tool_jobs.reconcile()
    await process.reattach()
    STATE.controller_connected = True
    _tasks[:] = [asyncio.create_task(_maintain()), asyncio.create_task(_monitor_storage())]


async def _maintain() -> None:
    while not _closing:
        try:
            await tool_jobs.reconcile()
            await process.reconcile()
        except Exception as exc:
            process.report_error(f"Background check failed: {exc}")
        await asyncio.sleep(2)


async def _monitor_storage() -> None:
    while not _closing:
        await storage.poll()
        await asyncio.sleep(2)


async def shutdown() -> None:
    global _closing
    _closing = True
    # Close the Start gate and latch the acquisition Stop before tool teardown yields.
    await process.shutdown()
    try:
        await tool_jobs.shutdown()
    except Exception as exc:
        process.report_error(f"Tool shutdown is not verified: {exc}")
    # The container grace period is the external deadline: never force-kill the
    # acquisition while its writers drain, nor release a camera tool early.
    while STATE.process_state in (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING) or STATE.snapshot_busy:
        await asyncio.sleep(0.2)
    for task in _tasks:
        task.cancel()
    await asyncio.gather(*_tasks, return_exceptions=True)
    TAILER.stop()
    ASTERX_LIVE.stop()
    STATE.controller_connected = False
    # Keep the flock until interpreter exit: device operations may still be unwinding.
