"""AmigaDrivers process lifecycle: preflight, start (attached), stop, reattach.

Docker-exec semantics that shape everything here:
  - killing the `docker exec` client does NOT touch the remote process — stop
    is a second exec running `pkill`; closing the GUI leaves acquisition alive
    (that is the reattach story, by design);
  - the client's exit status mirrors the remote one, so an ATTACHED launch
    yields the real exit code (0 graceful; 134 = uncaught fatal error).
"""

from __future__ import annotations

import asyncio
import re
import time
from collections import deque
from pathlib import Path

from ..constants import BIN_AMIGA, MAIN_CONFIG_CONTAINER, SESSION_DIR_RE
from ..state import STATE, ProcState
from . import config_store, docker_runner
from .health import MONITOR
from .log_buffer import BUFFER, parse_line
from .session_tailer import TAILER

PROCESS_NAME = "AmigaDrivers"
_SESSION_RE = re.compile(SESSION_DIR_RE)

# Wired once in main.py: TAILER feeds both the health monitor and the buffer.
launch_stderr: deque[str] = deque(maxlen=200)

_watcher_task: asyncio.Task | None = None
_exit_poll_task: asyncio.Task | None = None


def _cancel(task: asyncio.Task | None) -> None:
    if task is not None and not task.done():
        task.cancel()


def _session_dirs(output_dir: Path) -> set[str]:
    if not output_dir.is_dir():
        return set()
    return {p.name for p in output_dir.iterdir() if p.is_dir() and _SESSION_RE.match(p.name)}


def _log_file(session: Path) -> Path:
    return session / f"log_{session.name}.log"


async def preflight() -> tuple[list[str], list[str]]:
    """Returns (errors, warnings). Errors block the start."""
    errors: list[str] = []
    warnings: list[str] = []
    if not await docker_runner.is_container_up():
        errors.append("容器 amiga-sensor-dev 未运行")
        return errors, warnings
    if not await docker_runner.binary_exists(BIN_AMIGA):
        errors.append("未找到 build/bin/AmigaDrivers —— 请先在容器内编译（Build.bash）")
    if await docker_runner.pgrep(PROCESS_NAME):
        errors.append("AmigaDrivers 已在运行")
    settings = config_store.main_settings()
    errors.extend(config_store.output_dir_problems(settings["output_dir_raw"]))
    if not settings["enable_logging"]:
        warnings.append("Enable Logging 为 false：没有会话日志文件，GUI 的健康监控与日志页将失效")
    if not any(settings["enables"].values()):
        errors.append("没有任何传感器被启用（全部 Enable 为 false）")
    if STATE.snapshot_busy and settings["enables"].get("gox", False):
        errors.append("GoX 快照正在进行 —— 相机控制通道独占，请等它完成（≤30s）再启动")
    return errors, warnings


async def start() -> None:
    """Preflight must have passed (UI gates on it). Transitions IDLE -> RUNNING."""
    global _watcher_task
    # Synchronous check-and-set re-entrancy guard: a second click must bounce
    # BEFORE any await, and must not cancel a live run's watcher.
    if STATE.process_state in (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING):
        return
    STATE.process_state = ProcState.STARTING

    # Snapshot config + state synchronously (no await yet), so an Enable toggle
    # cannot land between the snapshot and the actual process launch — the UI
    # refuses toggles while STARTING.
    settings = config_store.main_settings()
    output_dir: Path | None = settings["output_dir"]
    if output_dir is None:  # preflight blocks this; defensive only
        STATE.process_state = ProcState.FAILED
        STATE.last_error = "Output Directory 不在容器挂载可见范围内"
        return
    known_sessions = _session_dirs(output_dir)
    STATE.exit_code = None
    STATE.last_error = ""
    STATE.active_session = None
    STATE.attached = True
    STATE.enables_at_start = dict(settings["enables"])
    STATE.pending_config_notice = False
    launch_stderr.clear()
    BUFFER.clear()
    MONITOR.reset(settings["enables"], config_store.lms_instance_names())

    _cancel(_watcher_task)
    _cancel(_exit_poll_task)

    # File caps for INS401 raw sockets / LMS SCHED_FIFO (mirrors Start.bash).
    # Must run as root — the container's default user cannot setcap.
    res = await docker_runner.exec_(
        ["setcap", "cap_net_raw,cap_sys_nice+ep", BIN_AMIGA], user="root", timeout=10
    )
    if not res.ok:
        BUFFER.append(parse_line(f"setcap failed (INS401 may abort): {res.stderr.strip()}",
                                 fallback_module="gui"))

    proc = await docker_runner.spawn([BIN_AMIGA, MAIN_CONFIG_CONTAINER])
    _watcher_task = asyncio.get_running_loop().create_task(
        _watch(proc, output_dir, known_sessions)
    )


async def _watch(proc: asyncio.subprocess.Process, output_dir: Path, known: set[str]) -> None:
    """Drains stderr, detects the new session dir, then waits for the exit code."""
    session_found = asyncio.Event()

    async def drain_stderr() -> None:
        assert proc.stderr is not None
        async for bline in proc.stderr:
            line = parse_line(bline.decode(errors="replace"), fallback_module="stderr")
            launch_stderr.append(line.raw)
            # Once the file log is live the tailer is the primary feed; stderr
            # lines would duplicate it, so only forward them before that.
            if not session_found.is_set():
                BUFFER.append(line)

    async def detect_session() -> None:
        warned_at = time.monotonic() + 30.0
        while True:
            fresh = _session_dirs(output_dir) - known
            if fresh:
                session = output_dir / sorted(fresh)[-1]
                STATE.active_session = session
                STATE.session_started_at = time.time()
                TAILER.start(_log_file(session), replay=False)
                # stop() may already have flipped to STOPPING — don't undo it
                if STATE.process_state is ProcState.STARTING:
                    STATE.process_state = ProcState.RUNNING
                session_found.set()
                return
            if proc.returncode is not None:
                return  # died before creating the session dir
            if warned_at is not None and time.monotonic() > warned_at:
                # Keep polling (slow mounts happen) but tell the user something
                # is off instead of silently sitting in STARTING.
                STATE.last_error = (
                    f"30s 内未在 {output_dir} 检测到会话目录 —— 进程仍在运行，继续等待"
                )
                warned_at = None
            await asyncio.sleep(0.5)

    drain = asyncio.get_running_loop().create_task(drain_stderr())
    detect = asyncio.get_running_loop().create_task(detect_session())
    code = await proc.wait()
    await asyncio.gather(drain, detect, return_exceptions=True)

    STATE.exit_code = code
    if code == 0:
        STATE.process_state = ProcState.EXITED
    else:
        STATE.process_state = ProcState.FAILED
        err = BUFFER.last_error_line()
        STATE.last_error = err.raw if err else "\n".join(list(launch_stderr)[-5:])


async def stop(term_timeout: float = 15.0) -> None:
    """SIGTERM the remote process, escalate to SIGKILL after the timeout."""
    if STATE.process_state not in (ProcState.RUNNING, ProcState.STARTING):
        return
    STATE.process_state = ProcState.STOPPING
    await docker_runner.pkill(PROCESS_NAME, "TERM")
    deadline = time.monotonic() + term_timeout
    while time.monotonic() < deadline:
        if not await docker_runner.pgrep(PROCESS_NAME):
            break
        await asyncio.sleep(0.5)
    else:
        BUFFER.append(parse_line("graceful stop timed out — sending SIGKILL", fallback_module="gui"))
        await docker_runner.pkill(PROCESS_NAME, "KILL")
        while await docker_runner.pgrep(PROCESS_NAME):
            await asyncio.sleep(0.5)
    # The attached watcher (if any) records the exit code; for detached runs
    # the exit poller finishes the transition.
    if not STATE.attached and STATE.process_state is ProcState.STOPPING:
        STATE.process_state = ProcState.EXITED


async def reattach() -> None:
    """GUI startup: adopt an already-running acquisition (attached=False —
    the exit code is unknowable; a clean stop is inferred from the
    'All drivers shut down' marker)."""
    global _exit_poll_task
    if not await docker_runner.is_container_up():
        return
    if not await docker_runner.pgrep(PROCESS_NAME):
        return

    settings = config_store.main_settings()
    output_dir: Path | None = settings["output_dir"]
    sessions = sorted(_session_dirs(output_dir)) if output_dir is not None else []
    STATE.process_state = ProcState.RUNNING
    STATE.attached = False
    STATE.enables_at_start = dict(settings["enables"])  # best effort (live file)
    MONITOR.reset(settings["enables"], config_store.lms_instance_names())
    if sessions and output_dir is not None:
        session = output_dir / sessions[-1]
        STATE.active_session = session
        STATE.session_started_at = session.stat().st_mtime
        TAILER.start(_log_file(session), replay=True)

    async def poll_exit() -> None:
        while await docker_runner.pgrep(PROCESS_NAME):
            await asyncio.sleep(2.0)
        # Give the 0.5s tailer a moment to ingest the final "All drivers shut
        # down" marker, and let stop() finish its own EXITED transition first.
        await asyncio.sleep(1.5)
        if STATE.process_state not in (ProcState.RUNNING, ProcState.STOPPING):
            return  # stop() (or a newer start) already finalized the state
        STATE.exit_code = None
        STATE.process_state = ProcState.EXITED if MONITOR.clean_stop else ProcState.FAILED
        if not MONITOR.clean_stop:
            err = BUFFER.last_error_line()
            STATE.last_error = err.raw if err else "进程已退出（退出码未知）"

    _exit_poll_task = asyncio.get_running_loop().create_task(poll_exit())
