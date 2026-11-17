"""Process primitives; the GUI, the acquisition and the device tools share one Linux container."""

from __future__ import annotations

import asyncio
import os
import signal as signals
from dataclasses import dataclass
from pathlib import Path

from ..constants import REPO_ROOT, RUNTIME_DIR

_BOOT_ID = Path("/proc/sys/kernel/random/boot_id")


@dataclass(frozen=True)
class ExecResult:
    code: int
    stdout: str
    stderr: str

    @property
    def ok(self) -> bool:
        return self.code == 0


async def exec_(args: list[str], *, timeout: float | None = None) -> ExecResult:
    """Run a local utility to completion with bounded waiting."""
    try:
        proc = await asyncio.create_subprocess_exec(
            *args, cwd=REPO_ROOT, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
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
    """Launch acquisition in its own process group, so a terminal signal cannot
    bypass the owner's orderly stop; reattachment handles an interrupted GUI."""
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    return await asyncio.create_subprocess_exec(
        *args, cwd=REPO_ROOT,
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE,
        env={**os.environ, "AMIGA_STATUS_FILE": str(RUNTIME_DIR / "acquisition.json")},
        start_new_session=True,
    )


async def gated_tool(args: list[str]) -> asyncio.subprocess.Process:
    # The PID survives exec; tool_jobs journals it before sending GO.
    gate = 'printf "%s\\n" "$$"; IFS= read -r permit && [ "$permit" = GO ] && exec "$@"'
    return await asyncio.create_subprocess_exec(
        "/bin/sh", "-c", gate, "amiga-tool", *args, cwd=REPO_ROOT,
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
    """PID, start time and boot id: a later process that reuses the PID never matches."""
    pid: int
    start_ticks: str
    boot_id: str


def process_identity(pid: int) -> ProcessIdentity:
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except FileNotFoundError:
        raise ProcessLookupError(f"Process {pid} has exited") from None
    boot_id = _BOOT_ID.read_text().strip()
    # comm (field 2) may contain spaces or parentheses. Field 22 is starttime.
    fields = stat.rsplit(")", 1)[-1].split()
    if len(fields) < 20 or not fields[19].isdigit() or not boot_id:
        raise RuntimeError("Invalid process identity")
    if fields[0] in ("Z", "X"):
        raise ProcessLookupError(f"Process {pid} has exited")
    return ProcessIdentity(pid, fields[19], boot_id)


async def running_process(name: str) -> ProcessIdentity | None:
    result = await exec_(["pgrep", "-x", name], timeout=5)
    if result.code == 1:
        return None
    pids = result.stdout.split()
    if not result.ok or len(pids) != 1 or not pids[0].isdigit():
        raise RuntimeError(f"Cannot identify a single {name} process")
    try:
        return process_identity(int(pids[0]))
    except ProcessLookupError:
        return None


def is_process_alive(ref: ProcessIdentity) -> bool:
    try:
        return process_identity(ref.pid) == ref
    except ProcessLookupError:
        return False


def signal_process(ref: ProcessIdentity, *, signal: str = "TERM") -> ExecResult:
    """Signal exactly the process ``ref`` identifies."""
    if signal not in ("TERM", "KILL"):
        raise ValueError("Unsupported stop signal")
    if not is_process_alive(ref):
        return ExecResult(1, "", "Process has already exited")
    try:
        os.kill(ref.pid, signals.SIGTERM if signal == "TERM" else signals.SIGKILL)
    except OSError as error:
        return ExecResult(1, "", str(error))
    return ExecResult(0, "", "")
