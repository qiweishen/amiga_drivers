"""Acquisition lifecycle owned by the service, independent of browser pages."""

from __future__ import annotations

import asyncio
import time
from collections import deque
from copy import deepcopy
from dataclasses import replace

from ..constants import BIN_AMIGA, MAIN_CONFIG
from ..state import STATE, ProcState, RecordingSnapshot
from . import config_store, runtime, session_info, tool_jobs, control_owner, run_status
from .asterx_live import LIVE as ASTERX_LIVE
from .driver_stats import STATS
from .log_buffer import BUFFER, parse_line
from .session_tailer import TAILER

PROCESS_NAME = "AmigaDrivers"
_ACTIVE = (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING)


class AcquisitionController:
    """Own acquisition tasks, process identity and the recording snapshot.

    All transitions run on the GUI event loop. Device tools, storage and log
    decoding remain separate services; pages call the narrow module facade.
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
        self.launch_stderr: deque[str] = deque(maxlen=200)

    def snapshot(self) -> RecordingSnapshot:
        # Copies isolate dictionaries as well as the frozen scalar fields.
        return deepcopy(self._state)

    def snapshot_value(self, name: str):
        # Frequent scalar reads must not copy the complete final-results JSON.
        return deepcopy(getattr(self._state, name))

    def _update(self, **changes) -> None:
        self._state = replace(self._state, **changes)

    def _report(self, message: str) -> None:
        if self._state.last_error != message:
            self._update(last_error=message)
            BUFFER.append(parse_line(message, fallback_module="gui"))

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
        self.launch_stderr.clear()
        STATE.sensors = {}
        return self._state.session_generation

    def _finish(self, code: int | None, *, clean: bool) -> None:
        if self._state.active_session is not None:
            try:
                doc = run_status.read_session(self._state.active_session, self._ref)
                self._apply_status(doc)
            except Exception:
                clean = False
                self._update(run_result={})
        if clean:
            detail = "Final recording result verified" if self._state.active_session else "Cancelled before launch"
        else:
            detail = "Recording failed or final result unavailable"
        self._update(exit_code=code, process_state=ProcState.EXITED if clean else ProcState.FAILED,
                     config_locked=False, control_uncertain=False, stop_requested=False, status_detail=detail)
        if clean:
            self._update(last_error="")
        if not clean:
            err = BUFFER.last_error_line()
            if err is not None:
                # This diagnostic is already in the feed. Expose it in the
                # status summary without replaying it as another log event.
                self._update(last_error=err.raw)
            else:
                self._report("Recording failed or its final integrity result is unavailable")

    async def preflight(self) -> tuple[list[str], list[str]]:
        errors: list[str] = []
        warnings: list[str] = []
        try:
            env_ok, detail = await runtime.env_check()
            if not env_ok:
                return [detail], warnings
            if not await runtime.binary_exists(BIN_AMIGA):
                errors.append("build/bin/AmigaDrivers not found in the execution environment")
            if await runtime.pgrep(PROCESS_NAME):
                errors.append("AmigaDrivers is already running")
            await tool_jobs.check_idle()
            settings = config_store.main_settings()
            for driver, enabled in settings["enables"].items():
                if enabled:
                    config = config_store.get(driver)
                    if not config.path.is_file():
                        errors.append(f"{driver} config is not readable from the GUI: {config.path}")
            errors.extend(config_store.output_dir_problems(settings["output_dir_raw"]))
            if not any(settings["enables"].values()):
                errors.append("No sensor is enabled")
            if STATE.snapshot_busy or STATE.tool_uncertain:
                errors.append("A camera tool is in progress; wait for it to finish")
        except Exception as e:
            errors.append(f"Preflight failed: {e}")
        return errors, warnings

    async def start(self) -> bool:
        """Reserve startup synchronously; page cancellation cannot abandon the launch."""
        control_owner.require()
        if (self._closing or self._state.process_state in _ACTIVE or STATE.snapshot_busy or STATE.tool_uncertain or self._state.control_uncertain
                or self._adopting or (self._start_task is not None and not self._start_task.done())):
            return False
        generation = self._reset_run()
        self._update(attached=True)
        STATE.pending_config_notice = False
        self._start_task = self._background(self._launch(generation))
        return await asyncio.shield(self._start_task)

    async def _launch(self, generation: int) -> bool:
        spawning = False
        try:
            # The earlier page preflight may have preceded a confirmation dialog.
            # Repeat it while STARTING reserves the rig and config saves are locked.
            errors, _warnings = await self.preflight()
            if errors:
                raise RuntimeError("; ".join(errors))
            if self._state.stop_requested:
                self._finish(None, clean=True)  # cancelled before a process was created
                return False
            settings = config_store.main_settings()
            self._update(enables_at_start=dict(settings["enables"]))
            STATE.sensors = run_status.sensor_statuses(settings["enables"], config_store.lms_instance_names())
            # Runtime privileges are provisioned by the deployment, never mutated
            # as a side effect of pressing Start.
            if self._state.stop_requested:
                self._finish(None, clean=True)
                return False
            spawning = True
            self._proc = await runtime.spawn([runtime.exec_path(BIN_AMIGA), runtime.exec_path(MAIN_CONFIG)])
            self._watcher_task = self._background(self._watch(self._proc, generation))
            # If Stop arrived during spawn, its durable worker will find the child
            # after creation.
            return not self._state.stop_requested
        except asyncio.CancelledError:
            # Server shutdown may cancel even service tasks. Do not certify an
            # in-flight spawn as absent; startup recovery must inspect it next time.
            self._update(control_uncertain=spawning)
            if not spawning:
                self._finish(None, clean=True)
            raise
        except Exception as e:
            self._report(f"Could not start recording: {e}")
            self._update(process_state=ProcState.FAILED)
            self._update(config_locked=True)
            # A concurrent external start must not expose idle controls until
            # process absence is verified.
            self._update(control_uncertain=True)
            try:
                await self.reattach()
            except Exception as recovery_error:
                self._report(f"Start failed; process state is unknown: {recovery_error}")
            return False

    def _apply_status(self, doc: dict) -> None:
        parsed = run_status.sensor_update(doc, STATE.sensors)
        for key, (state, error) in parsed.items():
            STATE.sensors[key].state = state
            STATE.sensors[key].last_error = error
        lifecycle = doc["lifecycle"]
        phase = lifecycle["phase"]
        self._update(status_source=run_status.SCHEMA, status_detail=phase,
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
            info = await session_info.recover(ref)
            if generation != self._state.session_generation or ref != self._ref:
                return
            doc = run_status.read_session(info.path, ref)
            self._update(active_session=info.path, session_started_at=info.started_at,
                    enables_at_start=dict(info.enables), ownership_verified=True)
            STATE.sensors = run_status.sensor_statuses(info.enables, info.lms_names)
            STATS.reset()
            TAILER.start(info.path / "raw" / f"log_{info.path.name}.log", replay=True)
            ASTERX_LIVE.start(info.path, replay=True)
            self._update(last_error="")
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
                if generation != self._state.session_generation:
                    return
                line = parse_line(data.decode(errors="replace"), fallback_module="stderr")
                self.launch_stderr.append(line.raw)
                if not TAILER.ready:
                    BUFFER.append(line)

        drain = self._background(drain_stderr())
        try:
            while proc.returncode is None and generation == self._state.session_generation:
                try:
                    if self._ref is None:
                        ref = await runtime.process_identity(proc.pid)
                        if generation != self._state.session_generation:
                            return
                        self._ref = ref
                    if self._ref is not None:
                        await self._observe_session(self._ref, generation)
                except Exception as e:
                    if generation == self._state.session_generation:
                        self._update(control_uncertain=True)
                        self._update(config_locked=True)
                        self._update(status_detail="Status not verified")
                        self._report(f"Waiting for verified session metadata: {e}")
                await asyncio.sleep(0.5)
            if generation != self._state.session_generation:
                return
            code = await proc.wait()
            await drain
            if generation != self._state.session_generation:
                return
            clean = code == 0
            if self._state.active_session is not None:
                clean = clean and session_info.finished_cleanly(self._state.active_session, self._ref)
            elif code == 0:
                clean = False  # No identified session means there is no verifiable final result.
            self._finish(code, clean=clean)
        except Exception as e:
            if generation != self._state.session_generation:
                return
            self._update(control_uncertain=True)
            self._report(f"Acquisition monitoring failed: {e}")
            self._exit_poll_task = self._background(self._recover_after_disconnect(generation))
        finally:
            if not drain.done():
                drain.cancel()

    async def _recover_after_disconnect(self, generation: int) -> None:
        while generation == self._state.session_generation:
            try:
                ref = await runtime.running_process(PROCESS_NAME)
                if generation != self._state.session_generation:
                    return
                if ref is None:
                    self._finish(None, clean=False)
                    return
                # Do not carry the old run's metadata over to a different process.
                if ref != self._ref:
                    self._reset_run()
                    generation = self._state.session_generation
                self._ref = ref
                self._update(attached=False)
                self._update(control_uncertain=False)
                await self._poll_detached(ref, generation)
                return
            except Exception as e:
                if generation == self._state.session_generation:
                    self._update(control_uncertain=True)
                    self._report(f"Acquisition state is unknown: {e}")
            await asyncio.sleep(2.0)

    async def stop(self, term_timeout: float = 60.0) -> None:
        """Latch the request immediately; retain it across every launch await."""
        control_owner.require()
        if self._state.process_state not in _ACTIVE:
            return
        self._update(stop_requested=True)
        self._update(process_state=ProcState.STOPPING)
        if self._stop_task is None or self._stop_task.done():
            self._stop_task = self._background(self._stop_when_ready(self._state.session_generation, term_timeout))

    async def shutdown(self) -> None:
        """Reject new starts before latching the server's stop request."""
        self._closing = True
        await self.stop()

    async def _stop_when_ready(self, generation: int, term_timeout: float) -> None:
        deadline = time.monotonic() + term_timeout
        signalled: runtime.ProcessIdentity | None = None
        warned = False
        while generation == self._state.session_generation and self._state.stop_requested:
            # Never signal before this launch has either been cancelled or created
            # its child. In particular, TERM-before-spawn must not count as success.
            if self._start_task is not None and not self._start_task.done():
                await asyncio.sleep(0.1)
                continue
            try:
                if self._ref is None and self._proc is not None and self._proc.returncode is None:
                    ref = await runtime.process_identity(self._proc.pid)
                    if generation != self._state.session_generation or not self._state.stop_requested:
                        return
                    self._ref = ref
                ref = self._ref
                if ref is not None and signalled != ref:
                    result = await runtime.signal_process(ref)
                    if generation != self._state.session_generation or not self._state.stop_requested:
                        return
                    if result.ok:
                        signalled = ref
                    elif await runtime.is_process_alive(ref):
                        self._report(f"Stop request could not be delivered: {result.stderr.strip()}")
            except Exception as e:
                self._report(f"Stop is pending; cannot verify the acquisition process: {e}")
            if not warned and time.monotonic() >= deadline:
                self._report("Stop is still pending or shutdown is still draining; no forced termination was sent")
                warned = True
            await asyncio.sleep(0.5)

    async def reattach(self) -> None:
        """Use process identity and recorded metadata; never adopt the newest folder."""
        if self._adopting:
            return
        self._adopting = True
        try:
            env_ok, detail = await runtime.env_check()
            if not env_ok:
                raise RuntimeError(detail)
            ref = await runtime.running_process(PROCESS_NAME)
            if ref is None:
                self._update(control_uncertain=False)
                self._update(config_locked=False)
                return
            generation = self._reset_run()
            self._ref = ref
            self._update(process_state=ProcState.STARTING)
            self._update(attached=False)
            self._update(control_uncertain=False)
            self._exit_poll_task = self._background(self._poll_detached(ref, generation))
        except Exception as e:
            self._update(control_uncertain=True)
            self._update(config_locked=True)
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
        while generation == self._state.session_generation:
            try:
                alive = await runtime.is_process_alive(ref)
                if generation != self._state.session_generation:
                    return
                if not alive:
                    break
                self._update(control_uncertain=False)
            except Exception as e:
                if generation != self._state.session_generation:
                    return
                self._update(control_uncertain=True)
                self._report(f"Acquisition state is unknown: {e}")
                await asyncio.sleep(1.0)
                continue
            try:
                await self._observe_session(ref, generation)
            except Exception as e:
                self._update(control_uncertain=True)
                self._update(config_locked=True)
                self._report(f"Waiting for verified session metadata: {e}")
            await asyncio.sleep(1.0)
        if generation != self._state.session_generation:
            return
        # Let the tailer ingest final lines; the final manifest is the primary
        # integrity result, so a fixed log-poll delay cannot manufacture success.
        await asyncio.sleep(1.0)
        if generation != self._state.session_generation:
            return
        clean = False
        try:
            if self._state.active_session is not None:
                clean = session_info.finished_cleanly(self._state.active_session, self._ref)
        except Exception as e:
            self._report(f"Final recording status is unavailable: {e}")
        self._finish(None, clean=clean)


CONTROLLER = AcquisitionController()
STATE.bind_recording(CONTROLLER.snapshot, CONTROLLER.snapshot_value)

# Compatibility surface for existing pages and the service assembly.
preflight = CONTROLLER.preflight
start = CONTROLLER.start
stop = CONTROLLER.stop
reattach = CONTROLLER.reattach
reconcile = CONTROLLER.reconcile
shutdown = CONTROLLER.shutdown
report_error = CONTROLLER._report
snapshot = CONTROLLER.snapshot
