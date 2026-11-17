"""Acquisition lifecycle owned by the service, independent of browser pages.

Data-protecting rules kept here: stop sends TERM only (never KILL) and waits
for the writers to drain; a running acquisition is adopted only by its exact
process identity and its own manifest, never by "the newest folder"; and a
run counts as clean only when its final manifest says so.
"""

from __future__ import annotations

import asyncio
import os
import time
from dataclasses import replace

from . import config_store, runtime, session_info, tool_jobs, control_owner, run_status
from .asterx_live import LIVE as ASTERX_LIVE
from .driver_stats import STATS
from .log_buffer import BUFFER, parse_line
from .session_tailer import TAILER
from ..constants import BIN_AMIGA, MAIN_CONFIG
from ..state import STATE, ProcState, RecordingSnapshot

PROCESS_NAME = "AmigaDrivers"
_ACTIVE = (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING)


class AcquisitionController:
    """Own acquisition tasks, process identity and the recording snapshot.

    All transitions run on the GUI event loop. ``session_generation`` increases
    with every run, so a task left over from an earlier run never writes into
    the current one.
    """

    def __init__(self) -> None:
        self._state = RecordingSnapshot()
        self._start_task: asyncio.Task | None = None
        self._watcher_task: asyncio.Task | None = None
        self._exit_poll_task: asyncio.Task | None = None
        self._stop_task: asyncio.Task | None = None
        self._proc: asyncio.subprocess.Process | None = None
        self._ref: runtime.ProcessIdentity | None = None
        self._adopting = False
        self._closing = False

    def snapshot(self) -> RecordingSnapshot:
        return self._state

    def snapshot_value(self, name: str):
        return getattr(self._state, name)

    def _update(self, **changes) -> None:
        self._state = replace(self._state, **changes)

    def _current(self, generation: int) -> bool:
        return generation == self._state.session_generation

    def _report(self, message: str) -> None:
        if self._state.last_error != message:
            self._update(last_error=message)
            BUFFER.append(parse_line(message, fallback_module="gui"))

    def _metadata_unavailable(self, error: Exception) -> None:
        # Before the child publishes its first manifest, waiting is normal:
        # keep ownership unverified without logging it as an error.
        pending = isinstance(error, session_info.SessionMetadataPending)
        self._update(control_uncertain=True, config_locked=True,
                     status_detail=str(error) if pending else "Status not verified")
        if not pending:
            self._report(f"Waiting for verified session metadata: {error}")

    def _background(self, coro) -> asyncio.Task:
        task = asyncio.create_task(coro)
        task.add_done_callback(lambda done: None if done.cancelled() else done.exception())
        return task

    def _reset_run(self) -> int:
        if self._stop_task is not None and not self._stop_task.done():
            self._stop_task.cancel()
        self._stop_task = None
        self._proc = None
        self._ref = None
        self._state = RecordingSnapshot(
            session_generation=self._state.session_generation + 1,
            process_state=ProcState.STARTING, config_locked=True,
            status_detail="Waiting for session metadata",
        )
        TAILER.stop()
        ASTERX_LIVE.stop()
        BUFFER.clear()
        STATS.reset()
        STATE.sensors = {}
        return self._state.session_generation

    def _finish(self, *, clean: bool) -> None:
        if self._state.active_session is not None:
            try:
                self._apply_status(run_status.read_session(self._state.active_session, self._ref))
            except Exception:
                clean = False
                self._update(run_result={})
        if clean:
            detail = "Final recording result verified" if self._state.active_session else "Cancelled before launch"
        else:
            detail = "Recording failed or final result unavailable"
        self._update(process_state=ProcState.EXITED if clean else ProcState.FAILED, config_locked=False,
                     control_uncertain=False, stop_requested=False, status_detail=detail)
        if clean:
            self._update(last_error="")
            return
        err = BUFFER.last_error_line()
        if err is not None:
            # Already in the feed: show it in the summary without logging it again.
            self._update(last_error=err.raw)
        else:
            self._report("Recording failed or its final integrity result is unavailable")

    async def preflight(self) -> list[str]:
        errors: list[str] = []
        try:
            if not os.access(BIN_AMIGA, os.X_OK):
                errors.append("Acquisition program not found; build it first")
            if await runtime.pgrep(PROCESS_NAME):
                errors.append("Another acquisition process is already running")
            await tool_jobs.check_idle()
            settings = config_store.main_settings()
            for driver, enabled in settings["enables"].items():
                if enabled and not config_store.get(driver).path.is_file():
                    errors.append(f"{driver} config is not readable: {config_store.get(driver).path}")
            errors.extend(config_store.output_dir_problems(settings["output_dir_raw"]))
            if not any(settings["enables"].values()):
                errors.append("No sensor is enabled")
            if STATE.snapshot_busy or STATE.tool_uncertain:
                errors.append("A camera tool is in progress; wait for it to finish")
        except Exception as e:
            errors.append(f"Preflight failed: {e}")
        return errors

    async def start(self) -> bool:
        """Reserve startup synchronously; page cancellation cannot abandon the launch."""
        control_owner.require()
        busy = (self._closing or self._state.process_state in _ACTIVE or STATE.snapshot_busy
                or STATE.tool_uncertain or self._state.control_uncertain or self._adopting
                or (self._start_task is not None and not self._start_task.done()))
        if busy:
            return False
        generation = self._reset_run()
        self._update(attached=True)
        STATE.pending_config_notice = False
        self._start_task = self._background(self._launch(generation))
        return await asyncio.shield(self._start_task)

    async def _launch(self, generation: int) -> bool:
        spawning = False
        try:
            # The page's preflight may have preceded a dialog: repeat it while
            # STARTING reserves the rig and config saves are locked.
            errors = await self.preflight()
            if errors:
                raise RuntimeError("; ".join(errors))
            if self._state.stop_requested:
                self._finish(clean=True)  # cancelled before a process was created
                return False
            settings = config_store.main_settings()
            self._update(enables_at_start=dict(settings["enables"]))
            STATE.sensors = run_status.sensor_statuses(settings["enables"], config_store.lms_instance_names())
            spawning = True
            self._proc = await runtime.spawn([str(BIN_AMIGA), str(MAIN_CONFIG)])
            self._watcher_task = self._background(self._watch(self._proc, generation))
            # A Stop that arrived during spawn finds the child through its own worker.
            return not self._state.stop_requested
        except asyncio.CancelledError:
            # Server shutdown may cancel this task: an in-flight spawn is not
            # certified absent; startup recovery inspects it next time.
            self._update(control_uncertain=spawning)
            if not spawning:
                self._finish(clean=True)
            raise
        except Exception as e:
            self._report(f"Could not start recording: {e}")
            # Keep controls locked until the absence of a concurrent external start is verified.
            self._update(process_state=ProcState.FAILED, config_locked=True, control_uncertain=True)
            try:
                await self.reattach()
            except Exception as recovery_error:
                self._report(f"Start failed; process state is unknown: {recovery_error}")
            return False

    def _apply_status(self, doc: dict) -> None:
        for key, (state, error) in run_status.sensor_update(doc, STATE.sensors).items():
            STATE.sensors[key].state = state
            STATE.sensors[key].last_error = error
        lifecycle = doc["lifecycle"]
        phase = lifecycle["phase"]
        self._update(status_detail=phase,
                     run_result={"run": doc["run"], "driver_results": doc.get("driver_results", [])})
        if phase == "running" and lifecycle["configuration_read"]:
            self._update(config_locked=False)
            if not self._state.stop_requested:
                self._update(process_state=ProcState.RUNNING)
        elif phase in ("stopping", "finished"):
            self._update(process_state=ProcState.STOPPING, config_locked=True)
        else:
            self._update(config_locked=True)

    async def _observe_session(self, ref: runtime.ProcessIdentity, generation: int) -> None:
        if self._state.active_session is None:
            info = session_info.recover(ref)
            if not self._current(generation) or ref != self._ref:
                return
            doc = run_status.read_session(info.path, ref)
            self._update(active_session=info.path, session_started_at=info.started_at,
                         enables_at_start=dict(info.enables), ownership_verified=True, last_error="")
            STATE.sensors = run_status.sensor_statuses(info.enables, info.lms_names)
            STATS.reset()
            TAILER.start(info.path / "raw" / f"log_{info.path.name}.log", replay=True)
            ASTERX_LIVE.start(info.path, replay=True)
        else:
            doc = run_status.read_session(self._state.active_session, ref)
        self._apply_status(doc)
        self._update(control_uncertain=False)
        if self._state.last_error.startswith("Waiting for verified session metadata:"):
            self._update(last_error="")

    async def _watch(self, proc: asyncio.subprocess.Process, generation: int) -> None:
        async def drain_stderr() -> None:
            if proc.stderr is None:
                return
            async for data in proc.stderr:
                if not self._current(generation):
                    return
                if not TAILER.ready:  # the session log takes over once it exists
                    BUFFER.append_stderr(parse_line(data.decode(errors="replace"), fallback_module="stderr"))

        drain = self._background(drain_stderr())
        try:
            while proc.returncode is None and self._current(generation):
                try:
                    if self._ref is None:
                        self._ref = runtime.process_identity(proc.pid)
                    await self._observe_session(self._ref, generation)
                except Exception as e:
                    if self._current(generation):
                        self._metadata_unavailable(e)
                await asyncio.sleep(0.5)
            if not self._current(generation):
                return
            code = await proc.wait()
            await drain
            if not self._current(generation):
                return
            # Without an identified session there is no verifiable final result.
            clean = (code == 0 and self._state.active_session is not None
                     and session_info.finished_cleanly(self._state.active_session, self._ref))
            self._finish(clean=clean)
        except Exception as e:
            if not self._current(generation):
                return
            self._update(control_uncertain=True)
            self._report(f"Acquisition monitoring failed: {e}")
            self._exit_poll_task = self._background(self._recover_after_disconnect(generation))
        finally:
            if not drain.done():
                drain.cancel()

    async def _recover_after_disconnect(self, generation: int) -> None:
        while self._current(generation):
            try:
                ref = await runtime.running_process(PROCESS_NAME)
                if not self._current(generation):
                    return
                if ref is None:
                    self._finish(clean=False)
                    return
                # Do not carry the old run's metadata over to a different process.
                if ref != self._ref:
                    generation = self._reset_run()
                self._ref = ref
                self._update(attached=False, control_uncertain=False)
                await self._poll_detached(ref, generation)
                return
            except Exception as e:
                if self._current(generation):
                    self._update(control_uncertain=True)
                    self._report(f"Acquisition state is unknown: {e}")
            await asyncio.sleep(2.0)

    async def stop(self, term_timeout: float = 60.0) -> None:
        """Latch the request immediately; retain it across every launch await."""
        control_owner.require()
        if self._state.process_state not in _ACTIVE:
            return
        self._update(stop_requested=True, process_state=ProcState.STOPPING)
        if self._stop_task is None or self._stop_task.done():
            self._stop_task = self._background(self._stop_when_ready(self._state.session_generation, term_timeout))

    async def shutdown(self) -> None:
        """Reject new starts before latching the server's stop request."""
        self._closing = True
        await self.stop()

    async def _stop_when_ready(self, generation: int, term_timeout: float) -> None:
        """Send TERM once to the identified process and wait; never KILL acquisition writers."""
        deadline = time.monotonic() + term_timeout
        signalled: runtime.ProcessIdentity | None = None
        warned = False
        while self._current(generation) and self._state.stop_requested:
            # Never signal before this launch has been cancelled or created its
            # child: TERM-before-spawn must not count as success.
            if self._start_task is not None and not self._start_task.done():
                await asyncio.sleep(0.1)
                continue
            try:
                if self._ref is None and self._proc is not None and self._proc.returncode is None:
                    self._ref = runtime.process_identity(self._proc.pid)
                ref = self._ref
                if ref is not None and signalled != ref:
                    result = runtime.signal_process(ref)
                    if result.ok:
                        signalled = ref
                    elif runtime.is_process_alive(ref):
                        self._report(f"Stop request could not be delivered: {result.stderr.strip()}")
            except Exception as e:
                self._report(f"Stop is pending; cannot verify the acquisition process: {e}")
            if not warned and time.monotonic() >= deadline:
                self._report("Stop is still pending or shutdown is still draining; no forced termination was sent")
                warned = True
            await asyncio.sleep(0.5)

    async def reattach(self) -> None:
        """Adopt a running acquisition by process identity and its own manifest."""
        if self._adopting:
            return
        self._adopting = True
        try:
            ref = await runtime.running_process(PROCESS_NAME)
            if ref is None:
                self._update(control_uncertain=False, config_locked=False)
                return
            generation = self._reset_run()
            self._ref = ref
            self._update(process_state=ProcState.STARTING, attached=False, control_uncertain=False)
            self._exit_poll_task = self._background(self._poll_detached(ref, generation))
        except Exception as e:
            self._update(control_uncertain=True, config_locked=True)
            self._report(f"Cannot verify acquisition ownership: {e}")
        finally:
            self._adopting = False

    async def reconcile(self) -> None:
        """Retry an unavailable startup probe without changing an active monitor."""
        if (self._state.control_uncertain and not self._adopting
                and all(task is None or task.done()
                        for task in (self._start_task, self._watcher_task, self._exit_poll_task))):
            await self.reattach()

    async def _poll_detached(self, ref: runtime.ProcessIdentity, generation: int) -> None:
        while self._current(generation):
            try:
                alive = runtime.is_process_alive(ref)
            except Exception as e:
                self._update(control_uncertain=True)
                self._report(f"Acquisition state is unknown: {e}")
                await asyncio.sleep(1.0)
                continue
            if not alive:
                break
            self._update(control_uncertain=False)
            try:
                await self._observe_session(ref, generation)
            except Exception as e:
                if not self._current(generation):
                    return
                self._metadata_unavailable(e)
            await asyncio.sleep(1.0)
        if not self._current(generation):
            return
        # Let the tailer ingest the final lines; the final manifest decides the result.
        await asyncio.sleep(1.0)
        if not self._current(generation):
            return
        clean = False
        try:
            if self._state.active_session is not None:
                clean = session_info.finished_cleanly(self._state.active_session, self._ref)
        except Exception as e:
            self._report(f"Final recording status is unavailable: {e}")
        self._finish(clean=clean)


CONTROLLER = AcquisitionController()
STATE.bind_recording(CONTROLLER.snapshot_value)

# Module facade for pages and the service assembly.
preflight = CONTROLLER.preflight
start = CONTROLLER.start
stop = CONTROLLER.stop
reattach = CONTROLLER.reattach
reconcile = CONTROLLER.reconcile
shutdown = CONTROLLER.shutdown
report_error = CONTROLLER._report
