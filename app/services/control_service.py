"""Start and stop the single-container service assembly."""

from __future__ import annotations

import asyncio

from ..constants import RUNTIME_DIR
from ..state import STATE, ProcState
from . import control_owner, process, runtime, storage, tool_jobs
from .driver_stats import STATS
from .log_buffer import BUFFER
from .session_tailer import TAILER
from .asterx_live import LIVE as ASTERX_LIVE


class ApplicationServices:
    def __init__(self) -> None:
        self._poller: asyncio.Task | None = None
        self._storage_poller: asyncio.Task | None = None
        self._subscribed = False
        self._closing = False

    async def startup(self) -> None:
        self._closing = False
        RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
        control_owner.acquire()
        if not self._subscribed:
            TAILER.subscribe(STATS.on_line)
            TAILER.subscribe(BUFFER.append)
            self._subscribed = True
        STATE.env_ok, STATE.env_detail = await runtime.env_check()
        await tool_jobs.reconcile()
        await process.reattach()
        STATE.controller_connected = True
        self._poller = asyncio.create_task(self._maintain())
        self._storage_poller = asyncio.create_task(self._monitor_storage())

    async def _maintain(self) -> None:
        while not self._closing:
            try:
                STATE.env_ok, STATE.env_detail = await runtime.env_check()
                await tool_jobs.reconcile()
                await process.reconcile()
            except Exception as exc:
                STATE.env_ok = False
                STATE.env_detail = str(exc)
            await asyncio.sleep(2)

    async def _monitor_storage(self) -> None:
        while not self._closing:
            await storage.poll()
            await asyncio.sleep(2)

    async def shutdown(self) -> None:
        self._closing = True
        # Close the Start gate and latch acquisition Stop before tool teardown
        # yields. This call does not wait for acquisition writers to drain.
        await process.shutdown()
        try:
            await tool_jobs.shutdown()
        except Exception as exc:
            process.report_error(f"Tool shutdown is not verified: {exc}")
        # The container grace period is the external deadline. Do not force-kill
        # acquisition while its writers drain, or release a camera tool early.
        while STATE.process_state in (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING) or STATE.snapshot_busy:
            await asyncio.sleep(0.2)
        for task in (self._poller, self._storage_poller):
            if task is not None:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
        TAILER.stop()
        ASTERX_LIVE.stop()
        STATE.controller_connected = False
        # Keep the flock until interpreter exit: device operations may still be
        # unwinding after server cancellation and must not share their journal.


SERVICES = ApplicationServices()
startup = SERVICES.startup
shutdown = SERVICES.shutdown
