"""Execution backend abstraction: the C++ binaries either run inside the
devcontainer (docker exec) or natively on this host. Every mode decision —
process exec, pgrep/pkill, path namespace, environment health — lives here.

Mode selection (once, at GUI startup):
  AMIGA_GUI_MODE=docker|native forces a backend;
  auto (default): if the docker CLI can see the amiga-sensor-dev container
  (running or stopped) -> docker, otherwise -> native.
So a dev machine with the devcontainer keeps working unchanged, and a rig
without Docker (or without the container) transparently runs natively.
"""

from __future__ import annotations

import asyncio
import os
from pathlib import Path

from ..constants import CONTAINER, REPO_ROOT, to_container, to_host
from .docker_runner import ExecResult
from . import docker_runner

_mode: str = "docker"  # resolved by detect_mode() before any use


async def detect_mode() -> str:
    global _mode
    forced = os.environ.get("AMIGA_GUI_MODE", "auto").lower()
    if forced in ("docker", "native"):
        _mode = forced
        return _mode
    # auto: does docker know our container at all (running or not)?
    proc = await asyncio.create_subprocess_exec(
        "docker", "inspect", "-f", "{{.State.Status}}", CONTAINER,
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
    )
    try:
        code = await asyncio.wait_for(proc.wait(), timeout=5)
    except (asyncio.TimeoutError, FileNotFoundError):
        proc.kill()
        code = 1
    _mode = "docker" if code == 0 else "native"
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
    proc = await asyncio.create_subprocess_exec(
        *argv, cwd=str(REPO_ROOT),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
    )
    try:
        out, err = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        return ExecResult(-1, "", f"timeout after {timeout}s: {' '.join(argv)}")
    except FileNotFoundError as e:
        return ExecResult(127, "", str(e))
    return ExecResult(proc.returncode or 0, out.decode(errors="replace"), err.decode(errors="replace"))


async def spawn(args: list[str]) -> asyncio.subprocess.Process:
    """Attached launch for AmigaDrivers: stdout discarded (spinner noise),
    stderr piped. Stopping always goes through pkill(), never by killing the
    returned handle. start_new_session detaches the native child from the
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
    if is_docker():
        return await docker_runner.pgrep(name)
    res = await exec_(["pgrep", "-x", name], timeout=5)
    return res.ok


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
        return up, "" if up else f"容器 {CONTAINER} 未运行"
    return True, ""
