"""Async wrappers around the docker CLI (the GUI's only channel into the container)."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass

from ..constants import COMPOSE_FILE, CONTAINER


@dataclass
class ExecResult:
    code: int
    stdout: str
    stderr: str

    @property
    def ok(self) -> bool:
        return self.code == 0


async def _run(*argv: str, timeout: float | None = None) -> ExecResult:
    proc = await asyncio.create_subprocess_exec(
        *argv,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        out, err = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        return ExecResult(-1, "", f"timeout after {timeout}s: {' '.join(argv)}")
    return ExecResult(proc.returncode or 0, out.decode(errors="replace"), err.decode(errors="replace"))


async def exec_(args: list[str], *, workdir: str = "/workspace", user: str | None = None,
                timeout: float | None = None) -> ExecResult:
    """docker exec <container> <args> — argv passed verbatim (no shell)."""
    argv = ["docker", "exec", "-w", workdir]
    if user is not None:
        argv += ["-u", user]
    argv += [CONTAINER, *args]
    return await _run(*argv, timeout=timeout)


async def spawn(args: list[str], *, workdir: str = "/workspace") -> asyncio.subprocess.Process:
    """Attached docker exec: returns the live client process.

    stdout is discarded (only ActivitySpinner \\r noise lives there); stderr is
    piped for the caller to drain. NOTE: killing this client process does NOT
    kill the remote process — stopping requires a separate `pkill` exec.
    """
    return await asyncio.create_subprocess_exec(
        "docker", "exec", "-w", workdir, CONTAINER, *args,
        stdout=asyncio.subprocess.DEVNULL,
        stderr=asyncio.subprocess.PIPE,
    )


async def is_container_up() -> bool:
    res = await _run("docker", "inspect", "-f", "{{.State.Running}}", CONTAINER, timeout=5)
    return res.ok and res.stdout.strip() == "true"


async def compose_up() -> ExecResult:
    """Bring the devcontainer up (first run may build the image — long timeout)."""
    return await _run(
        "docker", "compose", "-f", str(COMPOSE_FILE), "up", "-d", timeout=600,
    )


async def pgrep(name: str) -> bool:
    """True if a process with this exact name runs inside the container."""
    res = await exec_(["pgrep", "-x", name], timeout=5)
    return res.ok


async def pkill(name: str, signal: str = "TERM") -> ExecResult:
    return await exec_(["pkill", f"-{signal}", "-x", name], timeout=5)


async def binary_exists(container_path: str) -> bool:
    res = await exec_(["test", "-x", container_path], timeout=5)
    return res.ok
