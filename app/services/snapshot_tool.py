"""Run a camera snapshot tool (jai_snapshot, fx10_snapshot) and read its result marker.

The C++ tools end stdout with ``SNAPSHOT: OK <dir>`` or ``SNAPSHOT: FAIL <code>
<reason>`` (ebus_driver/include/ebus_driver/tool_support.h) and exit with that
code. The marker is read before the exit code, so the page shows the tool's
own failure reason.
"""

from __future__ import annotations

import asyncio
import shutil
import time
from dataclasses import dataclass
from pathlib import Path

from . import tool_jobs
from ..constants import SNAPSHOT_KEEP

OK, FAIL = "SNAPSHOT: OK ", "SNAPSHOT: FAIL "


@dataclass(frozen=True)
class ToolRun:
    ok: bool
    reason: str = ""
    output_dir: Path | None = None  # the directory named by SNAPSHOT: OK
    raw_output: str = ""
    elapsed_s: float = 0.0


def joined(stdout: str, stderr: str) -> str:
    return (stdout + "\n--- stderr ---\n" + stderr).strip()


def marker(stdout: str) -> str:
    """The last ``SNAPSHOT: …`` line, or ""."""
    return next((line for line in reversed(stdout.splitlines()) if line.startswith("SNAPSHOT: ")), "")


def interpret(stdout: str, stderr: str, returncode: int | None, elapsed_s: float = 0.0) -> ToolRun:
    raw = joined(stdout, stderr)
    line = marker(stdout)
    if line.startswith(FAIL):
        return ToolRun(False, line[len(FAIL):], raw_output=raw, elapsed_s=elapsed_s)
    directory = line[len(OK):].strip() if line.startswith(OK) else ""
    if directory and returncode == 0:
        return ToolRun(True, output_dir=Path(directory), raw_output=raw, elapsed_s=elapsed_s)
    if returncode:
        reason = f"Snapshot tool exited with code {returncode}"
    else:
        reason = "Malformed SNAPSHOT result marker" if line else "No SNAPSHOT marker in the output"
    return ToolRun(False, reason, raw_output=raw, elapsed_s=elapsed_s)


async def run(argv: list[str], timeout_s: float) -> ToolRun:
    """Launch a registered tool (tool_jobs) and wait for its result; the caller holds the camera."""
    t0 = time.monotonic()
    proc = await tool_jobs.spawn(argv)
    try:
        out, err = await tool_jobs.communicate(proc, timeout_s)
    except asyncio.TimeoutError:
        return ToolRun(False, f"Timed out (>{timeout_s:.0f} s); {Path(argv[0]).name} was terminated",
                       elapsed_s=time.monotonic() - t0)
    return interpret(out.decode(errors="replace"), err.decode(errors="replace"), proc.returncode,
                     time.monotonic() - t0)


def new_output_dir(root: Path) -> Path:
    """<root>/<YYYYmmdd_HHMMSS>, with milliseconds added on a same-second collision."""
    name = time.strftime("%Y%m%d_%H%M%S")
    if (root / name).exists():
        name += f"_{int((time.time() % 1) * 1000):03d}"
    return root / name


def prune(root: Path, keep: int = SNAPSHOT_KEEP) -> None:
    """Keep only the newest ``keep`` temporary snapshot directories."""
    if root.is_dir():
        for stale in sorted(path for path in root.iterdir() if path.is_dir())[:-keep]:
            shutil.rmtree(stale, ignore_errors=True)
