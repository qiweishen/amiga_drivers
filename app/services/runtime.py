"""Process primitives for the GUI and acquisition in the same Linux container."""

from __future__ import annotations

import asyncio
import os
from dataclasses import dataclass
from pathlib import Path

from ..constants import REPO_ROOT, RUNTIME_DIR


@dataclass(frozen=True)
class ExecResult:
    code: int
    stdout: str
    stderr: str

    @property
    def ok(self) -> bool:
        return self.code == 0


def exec_path(path: Path | str) -> str:
    """GUI and binaries share one filesystem namespace."""
    return str(Path(path))


def to_host_path(path: str) -> Path:
    """A binary-emitted path is already in the GUI's namespace."""
    return Path(path)


def workdir() -> str:
    return str(REPO_ROOT)


async def exec_(args: list[str], *, timeout: float | None = None) -> ExecResult:
    """Run a local utility to completion with bounded waiting."""
    try:
        proc = await asyncio.create_subprocess_exec(
            *args, cwd=workdir(),
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
    except OSError as exc:
        return ExecResult(127, "", str(exc))
    try:
        out, err = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        try:
            proc.kill()
        except ProcessLookupError:
            pass
        await proc.wait()
        return ExecResult(-1, "", f"timeout after {timeout}s: {' '.join(args)}")
    return ExecResult(proc.returncode or 0, out.decode(errors="replace"), err.decode(errors="replace"))


async def spawn(args: list[str]) -> asyncio.subprocess.Process:
    """Launch acquisition; server shutdown requests an orderly stop/drain.

    A separate process group prevents a terminal signal bypassing the owner's
    stop sequence. Reattachment still handles an interrupted GUI server.
    """
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    return await asyncio.create_subprocess_exec(
        *args, cwd=workdir(),
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE,
        env={**os.environ, "AMIGA_STATUS_FILE": str(RUNTIME_DIR / "acquisition.json")},
        start_new_session=True,
    )


async def gated_tool(args: list[str]) -> asyncio.subprocess.Process:
    # The PID survives exec; tool_jobs journals it before sending GO.
    gate = 'printf "%s\\n" "$$"; IFS= read -r permit && [ "$permit" = GO ] && exec "$@"'
    return await asyncio.create_subprocess_exec(
        "/bin/sh", "-c", gate, "amiga-tool", *args, cwd=workdir(),
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        start_new_session=True,
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


async def process_identity(pid: int) -> ProcessIdentity:
    return await _identity(pid)


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


async def signal_process(ref: ProcessIdentity, *, signal: str = "TERM") -> ExecResult:
    if signal not in ("TERM", "KILL"):
        raise ValueError("Unsupported stop signal")
    if not await is_process_alive(ref):
        return ExecResult(1, "", "Acquisition process has already exited")
    return await exec_(["kill", f"-{signal}", "--", str(ref.pid)], timeout=5)


async def binary_exists(path: Path | str) -> bool:
    file = Path(path)
    return file.is_file() and os.access(file, os.X_OK)


async def env_check() -> tuple[bool, str]:
    """Preflight checks each executable in this process's environment."""
    return True, ""
