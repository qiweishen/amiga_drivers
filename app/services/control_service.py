"""Local control lifecycle, shared by the integrated GUI and standalone API."""

from __future__ import annotations

import asyncio
import uuid

from ..constants import RUNTIME_DIR
from ..state import STATE, ProcState
from . import control_owner, process, runtime, storage, tool_jobs
from .driver_stats import STATS
from .health import MONITOR
from .log_buffer import BUFFER
from .session_tailer import TAILER

_poller = None
_storage_poller = None
_subscribed = False
_closing = False


async def startup() -> None:
    global _poller, _storage_poller, _subscribed, _closing
    _closing = False
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    STATE.mode = await runtime.detect_mode()
    control_owner.acquire()
    STATE.controller_id = uuid.uuid4().hex
    STATE.controller_connected = True
    if not _subscribed:
        TAILER.subscribe(MONITOR.on_line)
        TAILER.subscribe(STATS.on_line)
        TAILER.subscribe(BUFFER.append)
        _subscribed = True
    STATE.env_ok, STATE.env_detail = await runtime.env_check()
    await tool_jobs.reconcile()
    await process.reattach()

    async def maintain() -> None:
        while not _closing:
            try:
                STATE.env_ok, STATE.env_detail = await runtime.env_check()
                await tool_jobs.reconcile()
                await process.reconcile()
            except Exception as exc:
                STATE.env_ok = False
                STATE.env_detail = str(exc)
            await asyncio.sleep(2)

    _poller = asyncio.create_task(maintain())
    async def monitor_storage():
        while not _closing:
            await storage.poll()
            await asyncio.sleep(2)
    _storage_poller = asyncio.create_task(monitor_storage())


async def shutdown(*, stop_recording: bool) -> None:
    global _closing
    _closing = True
    if stop_recording:
        try:
            await tool_jobs.shutdown()
        except Exception as exc:
            STATE.last_error = f"Tool shutdown is not verified: {exc}"
        await process.stop()
        # Container stop_grace_period is the external deadline. Do not certify
        # completion or force-kill acquisition while its writers are draining.
        while STATE.process_state in (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING) or STATE.snapshot_busy:
            await asyncio.sleep(0.2)
    for task in (_poller, _storage_poller):
        if task is not None:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
    TAILER.stop()
    # Keep the flock until interpreter exit. A cancelled page/tool task may
    # still be finalizing its journal while server shutdown unwinds; releasing
    # early would let another controller overwrite that same journal.
