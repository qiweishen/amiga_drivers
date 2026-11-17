"""Durable tool identity, bounded output, and verified local termination.

The shell is a fixed launch gate: its PID survives exec. No device tool runs
until that identity has been journaled. Arguments are always separate argv.
Only a registered tool can be force-stopped; acquisition uses process.stop.
"""

from __future__ import annotations

import asyncio
import json
import time
import uuid
from dataclasses import asdict, dataclass

from . import runtime, atomic_json
from ..constants import RUNTIME_DIR
from ..state import STATE

NAMES = ("jai_snapshot", "fx10_snapshot", "ebus_set_ip", "fx10_reference", "ebus_discover")
JOURNAL = RUNTIME_DIR / "tool-operation.json"
_handle = None
_stopping = False


@dataclass
class Handle:
    proc: asyncio.subprocess.Process
    identity: runtime.ProcessIdentity
    name: str


def _save() -> None:
    atomic_json.write(JOURNAL, STATE.tool_operation)


def begin(kind: str, driver: str | None) -> None:
    STATE.tool_operation = {"id": uuid.uuid4().hex, "kind": kind, "driver": driver,
                            "state": "starting", "started_ns": str(time.time_ns()), "error": ""}
    _save()


async def check_idle() -> None:
    for name in NAMES:
        if await runtime.pgrep(name):
            raise RuntimeError(f"{name} is still running; device tools remain locked")


async def spawn(args: list[str]) -> asyncio.subprocess.Process:
    global _handle
    from pathlib import Path
    name = Path(args[0]).name
    if _stopping or STATE.tool_operation.get("stop_requested"):
        raise RuntimeError("Tool launch cancelled by a stop request")
    if name not in NAMES or _handle is not None:
        raise RuntimeError("Unregistered or overlapping device tool")
    proc = await runtime.gated_tool(args)
    try:
        line = await asyncio.wait_for(proc.stdout.readline(), 10)
        if not line.strip().isdigit():
            raise RuntimeError("Tool launch did not provide a process identity")
        identity = runtime.process_identity(int(line))
        _handle = Handle(proc, identity, name)
        STATE.tool_operation.update(state="running", process=asdict(identity), executable=name)
        _save()  # A GUI/controller restart can recover this exact PID and birth time.
        if _stopping or STATE.tool_operation.get("stop_requested"):
            raise RuntimeError("Tool launch cancelled by a stop request")
        proc.stdin.write(b"GO\n")
        await proc.stdin.drain()
        proc.stdin.close()
        return proc
    except BaseException:
        if proc.stdin is not None:
            proc.stdin.close()  # The gate exits on EOF without starting the tool.
        if _handle is not None:
            await terminate(proc)
        else:
            try:
                await asyncio.wait_for(proc.wait(), 10)
            except Exception:
                STATE.tool_uncertain = True
        raise


async def _wait_absent(identity: runtime.ProcessIdentity, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while runtime.is_process_alive(identity):
        if time.monotonic() >= deadline:
            return False
        await asyncio.sleep(0.2)
    return True


async def _terminate_identity(identity: runtime.ProcessIdentity) -> None:
    if not runtime.is_process_alive(identity):
        return
    response = runtime.signal_process(identity)
    if not response.ok and runtime.is_process_alive(identity):
        raise RuntimeError("Tool termination request failed")
    if await _wait_absent(identity, 15):
        return
    response = runtime.signal_process(identity, signal="KILL")
    if (not response.ok and runtime.is_process_alive(identity)) or not await _wait_absent(identity, 5):
        raise RuntimeError("Tool exit could not be confirmed; controls remain locked")


async def terminate(proc: asyncio.subprocess.Process) -> None:
    if _handle is None or _handle.proc is not proc:
        raise RuntimeError("Cannot stop a tool without its registered process identity")
    STATE.tool_operation["state"] = "stopping"
    try:
        _save()
        await _terminate_identity(_handle.identity)
        await asyncio.wait_for(proc.wait(), 5)
    except BaseException:
        STATE.tool_uncertain = True
        raise


async def communicate(proc: asyncio.subprocess.Process, timeout: float) -> tuple[bytes, bytes]:
    done = asyncio.gather(proc.stdout.read(-1), proc.stderr.read(-1), proc.wait())  # -1: until EOF
    try:
        out, err, _ = await asyncio.wait_for(asyncio.shield(done), timeout)
        await release(proc)  # A multi-capture operation (exposure bracket) may launch the next tool.
        return out, err
    except BaseException:
        await terminate(proc)
        raise
    finally:
        if not done.done():
            try:
                await asyncio.wait_for(asyncio.shield(done), 5)
            except BaseException:
                done.cancel()
        await asyncio.gather(done, return_exceptions=True)


async def confirm_exit(proc: asyncio.subprocess.Process) -> None:
    if _handle is None or _handle.proc is not proc:
        raise RuntimeError("Unknown tool handle")
    if not await _wait_absent(_handle.identity, 1):
        raise RuntimeError("The tool connection ended while its process was still alive")


async def release(proc: asyncio.subprocess.Process) -> None:
    """Confirm that this tool has exited, then free the slot for the next tool of the same operation."""
    global _handle
    await confirm_exit(proc)
    _handle = None


async def finish(error: str = "") -> None:
    global _handle
    try:
        if _handle is not None:
            await confirm_exit(_handle.proc)
        await check_idle()
        STATE.tool_uncertain = False
        STATE.tool_operation.update(state="failed" if error else "completed", error=error,
                                    ended_ns=str(time.time_ns()))
    except Exception as exc:
        STATE.tool_uncertain = True
        STATE.tool_operation.update(state="unknown", error=str(exc))
    finally:
        _handle = None
        _save()


async def reconcile() -> None:
    """Recover surviving tools after restart; never signal an unowned name.

    Called sequentially (startup, then the 2 s maintenance loop)."""
    if STATE.snapshot_busy:
        return
    try:
        if not STATE.tool_operation and JOURNAL.exists():
            doc = json.loads(JOURNAL.read_text(encoding="utf-8"))
            if not isinstance(doc, dict):
                raise ValueError("Invalid tool journal")
            STATE.tool_operation = doc
        await check_idle()
        # A journaled gate can briefly exist before exec: check it as well.
        entry = STATE.tool_operation
        if entry.get("state") in ("starting", "running", "stopping", "unknown"):
            if entry.get("process") and runtime.is_process_alive(runtime.ProcessIdentity(**entry["process"])):
                raise RuntimeError("A previous tool operation is still running")
            entry.update(state="interrupted", error="Controller connection ended; inspect retained outputs",
                         ended_ns=str(time.time_ns()))
            _save()
        STATE.tool_uncertain = False
    except Exception as exc:
        STATE.tool_uncertain = True
        STATE.tool_operation["error"] = str(exc)


async def stop(operation_id: str) -> None:
    from .control_owner import require
    require()
    entry = STATE.tool_operation
    if entry.get("id") != operation_id:
        raise RuntimeError("The tool operation changed or its identity is unavailable")
    entry["stop_requested"] = True
    if not entry.get("process") and entry.get("state") == "starting":
        _save()
        return
    if entry.get("executable") not in NAMES or not entry.get("process"):
        raise RuntimeError("The tool operation has no verified process identity")
    identity = runtime.ProcessIdentity(**entry["process"])
    entry["state"] = "stopping"
    try:
        _save()
        await _terminate_identity(identity)
    except Exception:
        STATE.tool_uncertain = True
        raise


async def shutdown() -> None:
    global _stopping
    _stopping = True
    if STATE.tool_operation.get("state") in ("starting", "running", "stopping", "unknown"):
        await stop(STATE.tool_operation.get("id", ""))
