"""Acquisition lifecycle owned by the service, independent of browser pages."""

from __future__ import annotations

import asyncio
import time
from collections import deque

from ..constants import BIN_AMIGA, MAIN_CONFIG
from ..state import STATE, ProcState, SensorState
from . import config_store, runtime, session_info, tool_jobs, control_owner, run_status, control_client
from .asterx_live import LIVE as ASTERX_LIVE
from .driver_stats import STATS
from .health import MONITOR
from .log_buffer import BUFFER, parse_line
from .session_tailer import TAILER

PROCESS_NAME = "AmigaDrivers"
_ACTIVE = (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING)
launch_stderr: deque[str] = deque(maxlen=200)

_start_task: asyncio.Task | None = None
_watcher_task: asyncio.Task | None = None
_exit_poll_task: asyncio.Task | None = None
_stop_task: asyncio.Task | None = None
_proc: asyncio.subprocess.Process | None = None
_ref: runtime.ProcessIdentity | None = None
_adopting = False


def _report(message: str) -> None:
    if STATE.last_error != message:
        STATE.last_error = message
        BUFFER.append(parse_line(message, fallback_module="gui"))


def _background(coro) -> asyncio.Task:
    task = asyncio.create_task(coro)
    task.add_done_callback(lambda done: None if done.cancelled() else done.exception())
    return task


def _reset_run() -> int:
    global _proc, _ref, _stop_task
    if _stop_task is not None and not _stop_task.done():
        _stop_task.cancel()
    _stop_task = None
    _proc = None
    _ref = None
    STATE.session_generation += 1
    STATE.process_state = ProcState.STARTING
    STATE.stop_requested = False
    STATE.config_locked = True
    STATE.ownership_verified = False
    STATE.active_session = None
    STATE.session_started_at = None
    STATE.enables_at_start = {}
    STATE.exit_code = None
    STATE.last_error = ""
    STATE.status_source = "unknown"
    STATE.status_detail = "Waiting for session metadata"
    STATE.run_result = {}
    TAILER.stop()
    ASTERX_LIVE.stop()
    BUFFER.clear()
    STATS.reset()
    launch_stderr.clear()
    STATE.sensors = {}
    return STATE.session_generation


def _finish(code: int | None, *, clean: bool) -> None:
    if STATE.active_session is not None:
        try:
            doc = run_status.read_json(STATE.active_session / "raw" / "drivers.json")
            STATE.run_result = {"run": doc["run"], "driver_results": doc.get("driver_results", [])}
        except Exception:
            clean = False
            STATE.run_result = {}
    STATE.exit_code = code
    STATE.process_state = ProcState.EXITED if clean else ProcState.FAILED
    STATE.config_locked = False
    STATE.control_uncertain = False
    STATE.stop_requested = False
    STATE.status_detail = "Final recording result verified" if clean and STATE.active_session else (
        "Cancelled before launch" if clean else "Recording failed or final result unavailable")
    if clean:
        STATE.last_error = ""
    if not clean:
        err = BUFFER.last_error_line()
        _report(err.raw if err else "Recording failed or its final integrity result is unavailable")


async def preflight() -> tuple[list[str], list[str]]:
    if control_client.enabled():
        result = await control_client.call("recording.preflight")
        return result[0], result[1]
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
        if not settings["enable_logging"]:
            warnings.append("Logging is disabled: session recovery and health monitoring may be unavailable")
        if not any(settings["enables"].values()):
            errors.append("No sensor is enabled")
        if STATE.snapshot_busy or STATE.tool_uncertain:
            errors.append("A camera tool is in progress; wait for it to finish")
    except Exception as e:
        errors.append(f"Preflight failed: {e}")
    return errors, warnings


async def start() -> bool:
    """Reserve startup synchronously; page cancellation cannot abandon the launch."""
    global _start_task
    if control_client.enabled():
        return bool(await control_client.call("recording.start"))
    control_owner.require()
    if (STATE.process_state in _ACTIVE or STATE.snapshot_busy or STATE.tool_uncertain or STATE.control_uncertain
            or _adopting or (_start_task is not None and not _start_task.done())):
        return False
    generation = _reset_run()
    STATE.attached = True
    STATE.pending_config_notice = False
    _start_task = _background(_launch(generation))
    return await asyncio.shield(_start_task)


async def _launch(generation: int) -> bool:
    global _proc, _watcher_task
    spawning = False
    try:
        # The earlier page preflight may have preceded a confirmation dialog.
        # Repeat it while STARTING reserves the rig and config saves are locked.
        errors, _warnings = await preflight()
        if errors:
            raise RuntimeError("; ".join(errors))
        if STATE.stop_requested:
            _finish(None, clean=True)  # cancelled before a process was created
            return False
        settings = config_store.main_settings()
        STATE.enables_at_start = dict(settings["enables"])
        MONITOR.reset(settings["enables"], config_store.lms_instance_names())
        # Runtime privileges are provisioned by the deployment, never mutated
        # as a side effect of pressing Start.
        if STATE.stop_requested:
            _finish(None, clean=True)
            return False
        spawning = True
        _proc = await runtime.spawn([runtime.exec_path(BIN_AMIGA), runtime.exec_path(MAIN_CONFIG)])
        _watcher_task = _background(_watch(_proc, generation))
        # If Stop arrived during spawn, its durable worker will find the child
        # after creation (including a delayed docker-exec launch).
        return not STATE.stop_requested
    except asyncio.CancelledError:
        # Server shutdown may cancel even service tasks. Do not certify an
        # in-flight spawn as absent; startup recovery must inspect it next time.
        STATE.control_uncertain = spawning
        if not spawning:
            _finish(None, clean=True)
        raise
    except Exception as e:
        _report(f"Could not start recording: {e}")
        STATE.process_state = ProcState.FAILED
        STATE.config_locked = True
        # A failed docker client launch or an external concurrent start must
        # not expose idle controls until absence is verified.
        STATE.control_uncertain = True
        try:
            await reattach()
        except Exception as recovery_error:
            _report(f"Start failed; process state is unknown: {recovery_error}")
        return False


async def _observe_session(ref: runtime.ProcessIdentity, generation: int) -> None:
    if STATE.active_session is None:
        info = await session_info.recover(ref)
        if generation != STATE.session_generation or ref != _ref:
            return
        STATE.active_session = info.path
        STATE.session_started_at = info.started_at
        STATE.enables_at_start = dict(info.enables)
        STATE.ownership_verified = True
        MONITOR.reset(info.enables, info.lms_names)
        STATS.reset()
        initial_status = run_status.read_session(info.path, ref)
        STATE.status_source = run_status.SCHEMA if initial_status is not None else "legacy-log"
        # Initialization can have advanced before recovery; replay its markers.
        TAILER.start(info.path / "raw" / f"log_{info.path.name}.log", replay=True, markers=initial_status is None)
        ASTERX_LIVE.start(info.path, replay=True)
        STATE.last_error = ""
    doc = run_status.read_session(STATE.active_session, ref)
    if doc is not None:
        run_status.apply(doc)
        STATE.control_uncertain = False
        if STATE.last_error.startswith("Waiting for verified session metadata:"):
            STATE.last_error = ""
        return
    STATE.status_source = "legacy-log"
    STATE.status_detail = "Compatibility mode: lifecycle inferred from log markers"
    STATE.control_uncertain = False
    # A directory/manifest appears BEFORE Init() reads every driver config.
    # Keep saves locked until all enabled instances reported initialization.
    enabled = [st for st in STATE.sensors.values() if st.state is not SensorState.DISABLED]
    if (STATE.ownership_verified and enabled
            and all(st.state is SensorState.RUNNING for st in enabled)):
        STATE.config_locked = False
        if STATE.process_state is ProcState.STARTING and not STATE.stop_requested:
            STATE.process_state = ProcState.RUNNING


async def _watch(proc: asyncio.subprocess.Process, generation: int) -> None:
    global _ref, _exit_poll_task

    async def drain_stderr() -> None:
        if proc.stderr is None:
            return
        async for data in proc.stderr:
            if generation != STATE.session_generation:
                return
            line = parse_line(data.decode(errors="replace"), fallback_module="stderr")
            launch_stderr.append(line.raw)
            if not TAILER.ready:
                BUFFER.append(line)

    drain = _background(drain_stderr())
    try:
        while proc.returncode is None and generation == STATE.session_generation:
            try:
                if _ref is None:
                    ref = await runtime.running_process(PROCESS_NAME)
                    if generation != STATE.session_generation:
                        return
                    _ref = ref
                if _ref is not None:
                    await _observe_session(_ref, generation)
            except Exception as e:
                if generation == STATE.session_generation:
                    STATE.control_uncertain = True
                    STATE.config_locked = True
                    STATE.status_detail = "Status not verified"
                    _report(f"Waiting for verified session metadata: {e}")
            await asyncio.sleep(0.5)
        if generation != STATE.session_generation:
            return
        code = await proc.wait()
        await drain
        if generation != STATE.session_generation:
            return
        # An exec-client failure does not prove the remote acquisition exited.
        if code != 0:
            try:
                remaining = await runtime.running_process(PROCESS_NAME)
                if generation != STATE.session_generation:
                    return
            except Exception as e:
                STATE.control_uncertain = True
                _report(f"Lost the process connection; acquisition state is unknown: {e}")
                _exit_poll_task = _background(_recover_after_disconnect(generation))
                return
            if remaining is not None:
                if remaining != _ref:
                    _reset_run()
                    generation = STATE.session_generation
                _ref = remaining
                STATE.attached = False
                STATE.control_uncertain = False
                _exit_poll_task = _background(_poll_detached(remaining, generation))
                return
        clean = code == 0
        if STATE.active_session is not None:
            clean = clean and session_info.finished_cleanly(STATE.active_session)
        elif code == 0:
            clean = False  # No identified session means there is no verifiable final result.
        _finish(code, clean=clean)
    except Exception as e:
        if generation != STATE.session_generation:
            return
        STATE.control_uncertain = True
        _report(f"Acquisition monitoring failed: {e}")
        _exit_poll_task = _background(_recover_after_disconnect(generation))
    finally:
        if not drain.done():
            drain.cancel()


async def _recover_after_disconnect(generation: int) -> None:
    global _ref
    while generation == STATE.session_generation:
        try:
            ref = await runtime.running_process(PROCESS_NAME)
            if generation != STATE.session_generation:
                return
            if ref is None:
                _finish(None, clean=False)
                return
            # Do not carry the old run's metadata over to a different process.
            if ref != _ref:
                _reset_run()
                generation = STATE.session_generation
            _ref = ref
            STATE.attached = False
            STATE.control_uncertain = False
            await _poll_detached(ref, generation)
            return
        except Exception as e:
            if generation == STATE.session_generation:
                STATE.control_uncertain = True
                _report(f"Acquisition state is unknown: {e}")
        await asyncio.sleep(2.0)


async def stop(term_timeout: float = 60.0) -> None:
    """Latch the request immediately; retain it across every launch await."""
    global _stop_task
    if control_client.enabled():
        await control_client.call("recording.stop", term_timeout)
        return
    control_owner.require()
    if STATE.process_state not in _ACTIVE:
        return
    STATE.stop_requested = True
    STATE.process_state = ProcState.STOPPING
    if _stop_task is None or _stop_task.done():
        _stop_task = _background(_stop_when_ready(STATE.session_generation, term_timeout))


async def _stop_when_ready(generation: int, term_timeout: float) -> None:
    global _ref
    deadline = time.monotonic() + term_timeout
    signalled: runtime.ProcessIdentity | None = None
    warned = False
    while generation == STATE.session_generation and STATE.stop_requested:
        # Never signal before this launch has either been cancelled or created
        # its child. In particular, TERM-before-spawn must not count as success.
        if _start_task is not None and not _start_task.done():
            await asyncio.sleep(0.1)
            continue
        try:
            if _ref is None and _proc is not None and _proc.returncode is None:
                ref = await runtime.running_process(PROCESS_NAME)
                if generation != STATE.session_generation or not STATE.stop_requested:
                    return
                _ref = ref
            ref = _ref
            if ref is not None and signalled != ref:
                result = await runtime.signal_process(ref)
                if generation != STATE.session_generation or not STATE.stop_requested:
                    return
                if result.ok:
                    signalled = ref
                elif await runtime.is_process_alive(ref):
                    _report(f"Stop request could not be delivered: {result.stderr.strip()}")
        except Exception as e:
            _report(f"Stop is pending; cannot verify the acquisition process: {e}")
        if not warned and time.monotonic() >= deadline:
            _report("Stop is still pending or shutdown is still draining; no forced termination was sent")
            warned = True
        await asyncio.sleep(0.5)


async def reattach() -> None:
    """Use process identity and recorded metadata; never adopt the newest folder."""
    global _adopting, _ref, _exit_poll_task
    if _adopting:
        return
    _adopting = True
    try:
        env_ok, detail = await runtime.env_check()
        if not env_ok:
            raise RuntimeError(detail)
        ref = await runtime.running_process(PROCESS_NAME)
        if ref is None:
            STATE.control_uncertain = False
            STATE.config_locked = False
            return
        generation = _reset_run()
        _ref = ref
        STATE.process_state = ProcState.STARTING
        STATE.attached = False
        STATE.control_uncertain = False
        _exit_poll_task = _background(_poll_detached(ref, generation))
    except Exception as e:
        STATE.control_uncertain = True
        STATE.config_locked = True
        _report(f"Cannot verify acquisition ownership: {e}")
    finally:
        _adopting = False


async def reconcile() -> None:
    """Retry an unavailable startup probe without changing an active monitor."""
    if (STATE.control_uncertain and not _adopting
            and all(task is None or task.done()
                    for task in (_start_task, _watcher_task, _exit_poll_task))):
        await reattach()


async def _poll_detached(ref: runtime.ProcessIdentity, generation: int) -> None:
    while generation == STATE.session_generation:
        try:
            alive = await runtime.is_process_alive(ref)
            if generation != STATE.session_generation:
                return
            if not alive:
                break
            STATE.control_uncertain = False
        except Exception as e:
            if generation != STATE.session_generation:
                return
            STATE.control_uncertain = True
            _report(f"Acquisition state is unknown: {e}")
            await asyncio.sleep(1.0)
            continue
        try:
            await _observe_session(ref, generation)
        except Exception as e:
            STATE.control_uncertain = True
            STATE.config_locked = True
            _report(f"Waiting for verified session metadata: {e}")
        await asyncio.sleep(1.0)
    if generation != STATE.session_generation:
        return
    # Let the tailer ingest final lines; the final manifest is the primary
    # integrity result, so a fixed log-poll delay cannot manufacture success.
    await asyncio.sleep(1.0)
    if generation != STATE.session_generation:
        return
    clean = False
    try:
        if STATE.active_session is not None:
            clean = session_info.finished_cleanly(STATE.active_session)
    except Exception as e:
        _report(f"Final recording status is unavailable: {e}")
    _finish(None, clean=clean)
