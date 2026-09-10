"""Execution backend abstraction: the C++ binaries either run inside the
devcontainer (docker exec) or natively on this host. Every mode decision —
process exec, pgrep/pkill, path namespace, environment health — lives here.

Mode selection (once, at GUI startup):
  AMIGA_GUI_MODE=docker|native forces a backend;
  auto (default): if the docker CLI can see the amiga-drivers-dev container
  (running or stopped) -> docker, otherwise -> native.
So a dev machine with the devcontainer keeps working unchanged, and a rig
without Docker (or without the container) transparently runs natively.
"""

from __future__ import annotations

import asyncio
import os
from dataclasses import dataclass
from pathlib import Path

from ..constants import CONTAINER, REPO_ROOT, to_container, to_host
from .docker_runner import ExecResult
from . import docker_runner


_mode: str = "docker"      # optimistic default; detect_mode() overwrites it
_resolved: bool = False    # guards the cache — _mode alone can't say "unset"
_lock = asyncio.Lock()


async def _probe_docker() -> str:
    """One-shot probe: does docker know our container, running or not?"""
    try:
        proc = await asyncio.create_subprocess_exec(
            # --type container: without it, a same-named image could match
            "docker", "inspect", "--type", "container",
            "-f", "{{.State.Status}}", CONTAINER,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
    except OSError:
        # No docker CLI, or it won't spawn -> native by definition.
        return "native"

    try:
        code = await asyncio.wait_for(proc.wait(), timeout=5)
    except asyncio.TimeoutError:
        # Daemon hung. Give up and treat as native.
        try:
            proc.kill()
        except ProcessLookupError:
            pass           # exited on its own between timeout and kill
        await proc.wait()  # reap, no zombie
        return "native"

    # Only the exit code is usable here — stdout goes to DEVNULL.
    return "docker" if code == 0 else "native"


async def detect_mode() -> str:
    global _mode, _resolved
    if _resolved:
        return _mode

    async with _lock:
        if _resolved:  # another task resolved it while we waited
            return _mode

        forced = os.environ.get("AMIGA_GUI_MODE", "auto").strip().lower()
        _mode = forced if forced in ("docker", "native") else await _probe_docker()
        _resolved = True
        return _mode


def mode() -> str:
    return _mode


def is_docker() -> bool:
    return _mode == "docker"


# --- path namespace ----------------------------------------------------------

def exec_path(host_path: Path | str) -> str:
    """The path string the BINARY will see for a host path."""
    return to_container(host_path) if is_docker() else str(Path(host_path))


def to_host_path(path_str: str) -> Path:
    """Map a binary-emitted path (e.g. the SNAPSHOT: OK dir) back to the host."""
    return to_host(path_str) if is_docker() else Path(path_str)


def workdir() -> str:
    """Working directory of every launched binary. Relative config entries
    (e.g. Output Directory "./recordings") resolve against this on both
    backends: /workspace in docker == REPO_ROOT natively."""
    return "/workspace" if is_docker() else str(REPO_ROOT)


# --- process primitives ------------------------------------------------------

def _sudo_prefix() -> list[str]:
    # -n: never prompt — an interactive sudo would hang the GUI silently.
    return [] if os.geteuid() == 0 else ["sudo", "-n"]


async def exec_(args: list[str], *, root: bool = False, timeout: float | None = None) -> ExecResult:
    """Run a command to completion in the execution environment."""
    if is_docker():
        return await docker_runner.exec_(args, user="root" if root else None, timeout=timeout)
    argv = (_sudo_prefix() + args) if root else args
    try:
        proc = await asyncio.create_subprocess_exec(
            *argv, cwd=str(REPO_ROOT),
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
    except OSError as e:
        return ExecResult(127, "", str(e))
    try:
        out, err = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        try:
            proc.kill()
        except ProcessLookupError:
            pass
        await proc.wait()
        return ExecResult(-1, "", f"timeout after {timeout}s: {' '.join(argv)}")
    return ExecResult(proc.returncode or 0, out.decode(errors="replace"), err.decode(errors="replace"))


async def spawn(args: list[str]) -> asyncio.subprocess.Process:
    """Attached launch for AmigaDrivers: stdout discarded (spinner noise),
    stderr piped. Stopping targets the verified acquisition identity, never
    the docker-exec handle. start_new_session detaches the native child from the
    GUI's terminal process group so Ctrl+C on (or death of) the GUI does not
    take the acquisition down — matching the docker-exec semantics that the
    reattach story depends on."""
    if is_docker():
        return await docker_runner.spawn(args)
    return await asyncio.create_subprocess_exec(
        *args, cwd=str(REPO_ROOT),
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE,
        start_new_session=True,
    )


async def popen(args: list[str]) -> asyncio.subprocess.Process:
    """Short-lived tool launch with BOTH pipes captured (jai_snapshot marker
    parsing needs stdout)."""
    if is_docker():
        return await asyncio.create_subprocess_exec(
            "docker", "exec", "-w", "/workspace", CONTAINER, *args,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
    return await asyncio.create_subprocess_exec(
        *args, cwd=str(REPO_ROOT),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
    )


async def pgrep(name: str) -> bool:
    res = await exec_(["pgrep", "-x", name], timeout=5)
    if res.code not in (0, 1):
        raise RuntimeError(f"Cannot inspect {name}: {res.stderr.strip() or res.code}")
    return res.code == 0


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    start_ticks: str
    boot_id: str


async def _identity(pid: int) -> ProcessIdentity:
    stat = await exec_(["cat", f"/proc/{pid}/stat"], timeout=5)
    boot = await exec_(["cat", "/proc/sys/kernel/random/boot_id"], timeout=5)
    if not stat.ok or not boot.ok:
        raise RuntimeError("Cannot read acquisition process identity")
    # comm (field 2) may contain spaces or parentheses. Field 22 is starttime.
    fields = stat.stdout.rsplit(")", 1)[-1].split()
    if len(fields) < 20 or not fields[19].isdigit() or not boot.stdout.strip():
        raise RuntimeError("Invalid acquisition process identity")
    if fields[0] in ("Z", "X"):
        raise ProcessLookupError("The acquisition process has exited")
    return ProcessIdentity(pid, fields[19], boot.stdout.strip())


async def running_process(name: str) -> ProcessIdentity | None:
    result = await exec_(["pgrep", "-x", name], timeout=5)
    if result.code == 1:
        return None
    pids = result.stdout.split()
    if not result.ok or len(pids) != 1 or not pids[0].isdigit():
        raise RuntimeError(f"Cannot identify a single {name} process")
    try:
        return await _identity(int(pids[0]))
    except ProcessLookupError:
        return None


async def is_process_alive(ref: ProcessIdentity) -> bool:
    exists = await exec_(["test", "-d", f"/proc/{ref.pid}"], timeout=5)
    if exists.code == 1:
        return False
    if not exists.ok:
        raise RuntimeError("Cannot inspect acquisition process")
    try:
        return await _identity(ref.pid) == ref
    except ProcessLookupError:
        return False
    except RuntimeError:
        # A process may exit between test and cat. Other failures stay unknown.
        exists = await exec_(["test", "-d", f"/proc/{ref.pid}"], timeout=5)
        if exists.code == 1:
            return False
        raise


async def signal_process(ref: ProcessIdentity) -> ExecResult:
    if not await is_process_alive(ref):
        return ExecResult(1, "", "Acquisition process has already exited")
    return await exec_(["kill", "-TERM", "--", str(ref.pid)], timeout=5)


async def process_files(ref: ProcessIdentity) -> list[str]:
    """Read open-file targets in the same namespace as the acquisition.

    File capabilities can make /proc/PID/fd unreadable to the same UID. Try
    the existing noninteractive privileged backend only for this read.
    Failure is reported as unknown ownership, never as an idle camera.
    """
    args = ["find", f"/proc/{ref.pid}/fd", "-mindepth", "1", "-maxdepth", "1", "-printf", "%l\\n"]
    result = await exec_(args, timeout=5)
    if not result.ok:
        result = await exec_(args, root=True, timeout=5)
    if not result.ok or not await is_process_alive(ref):
        raise RuntimeError("Cannot verify the acquisition's open session files")
    return result.stdout.splitlines()


async def pkill(name: str, signal: str = "TERM") -> ExecResult:
    if is_docker():
        return await docker_runner.pkill(name, signal)
    return await exec_(["pkill", f"-{signal}", "-x", name], timeout=5)


async def binary_exists(host_path: Path | str) -> bool:
    if is_docker():
        return await docker_runner.binary_exists(to_container(host_path))
    p = Path(host_path)
    return p.is_file() and os.access(p, os.X_OK)


# --- environment health ------------------------------------------------------

async def env_check() -> tuple[bool, str]:
    """(ok, detail). Docker: the container must be running. Native: always ok
    (missing binaries are caught by preflight per-binary checks)."""
    if is_docker():
        up = await docker_runner.is_container_up()
        return up, "" if up else f"Container {CONTAINER} is not running"
    return True, ""
