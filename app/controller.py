"""Standalone controller: python -m app.controller. Uses existing FastAPI/uvicorn.

Only fixed domain commands are exposed. There is no shell, path-write, process
signal or Docker proxy endpoint. Hardware commands are retained independently
of HTTP clients and identified by a journaled request id.
"""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import inspect
import json
import math
import os
import re
import time
import types
from contextlib import asynccontextmanager
from pathlib import Path
from typing import get_args, get_origin, get_type_hints, Union

from fastapi import FastAPI, HTTPException, Request
import uvicorn

from .constants import RUNTIME_DIR
from .state import STATE
from .services import (atomic_json, config_store, control_client, control_service, control_token, ebus_tools, fx10_reference,
                       fx10_tools, gox_tools, process, tool_jobs, wire)
from .services.driver_stats import STATS

PROTOCOL = control_client.PROTOCOL
MAX_BODY = 1024 * 1024
MAX_RESULT = 8 * 1024 * 1024
MAX_JOBS = 32
JOBS = RUNTIME_DIR / "requests"
_tasks: dict[str, asyncio.Task] = {}
_accepting = False
_token = ""
_summaries: dict[str, dict] = {}


def _summary(job: dict) -> dict:
    summary = {key: job.get(key) for key in ("id", "action", "state", "error", "started_ns")}
    result = job.get("result")
    if isinstance(result, dict) and (result.get("ok") is False or result.get("error")):
        summary.update(state="failed", error=result.get("reason") or result.get("error") or result.get("message"))
    return summary


def _handlers() -> dict:
    return {"recording.preflight": process.preflight, "recording.start": process.start,
            "recording.stop": process.stop, "gox.snapshot": gox_tools.snapshot,
            "fx10.snapshot": fx10_tools.snapshot, "camera.discover": ebus_tools.discover,
            "camera.set_ip": ebus_tools.set_ip, "fx10.reference": fx10_reference.collect_reference,
            "tool.stop": tool_jobs.stop,
            **{"config." + name: getattr(config_store, name) for name in
               ("save", "set_enable", "apply_device_ip", "apply_gox_acquisition", "apply_fx10_acquisition")}}


def _path(job_id: str) -> Path:
    if not re.fullmatch(r"[0-9a-f]{32}", job_id):
        raise HTTPException(400, "Invalid request id")
    return JOBS / (job_id + ".json")


def _save(job: dict) -> None:
    path = _path(job["id"])
    atomic_json.write(path, wire.encode(job), limit=MAX_RESULT)
    _summaries[job["id"]] = _summary(job)


def _read(job_id: str) -> dict:
    try:
        with _path(job_id).open(encoding="utf-8") as stream:
            raw = stream.read(MAX_RESULT + 1)
    except FileNotFoundError:
        raise HTTPException(404, "Request unknown or expired; do not automatically resubmit a device command") from None
    if len(raw) > MAX_RESULT:
        raise HTTPException(500, "Invalid request journal")
    job = json.loads(raw)
    if job["state"] in ("accepted", "running") and job.get("controller_id") != STATE.controller_id:
        job.update(state="failed", error_type="InterruptedError",
                   error="Controller restarted; command outcome is unknown. Inspect acquisition/tool state and retained outputs.")
    return job


@asynccontextmanager
async def lifespan(_app):
    global _accepting, _token
    if control_client.enabled():
        raise RuntimeError("The controller cannot itself be configured as a remote GUI")
    _token = control_token.read(create=os.environ.get("AMIGA_CONTROLLER_BOOTSTRAP_TOKEN") == "1")
    JOBS.mkdir(parents=True, exist_ok=True)
    await control_service.startup()
    # Recover only the bounded journal once; state polling does not reread
    # multi-megabyte preview results from disk on every GUI refresh.
    for path in sorted(JOBS.glob("*.json"), key=lambda p: p.stat().st_mtime_ns)[-MAX_JOBS:]:
        try:
            job = _read(path.stem)
            _summaries[job["id"]] = _summary(job)
            if job["action"] == "fx10.reference" and job["state"] == "completed" and job.get("result"):
                fx10_reference._last_result = wire.restore(fx10_reference.ReferenceResult, job["result"])
        except (ValueError, KeyError, TypeError, OSError, HTTPException):
            continue
    _accepting = True
    try:
        yield
    finally:
        _accepting = False
        # Accepted tasks get a scheduling turn to acquire their reservations
        # before shutdown latches Stop. No new HTTP command can enter now.
        await asyncio.sleep(0)
        await control_service.shutdown(stop_recording=True)


api = FastAPI(title="Amiga acquisition controller", lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)


@api.middleware("http")
async def authenticate(request: Request, call_next):
    from starlette.responses import JSONResponse
    if request.url.path != "/healthz":
        supplied = request.headers.get("Authorization", "")
        if not _token or not hmac.compare_digest(supplied.encode(), ("Bearer " + _token).encode()):
            return JSONResponse({"detail": "Controller authentication required"}, status_code=401)
    return await call_next(request)


@api.get("/healthz")
async def health():
    if not _accepting:
        raise HTTPException(503, "Controller is starting or stopping")
    return {"protocol": PROTOCOL, "ready": True}


@api.get("/v1/state")
async def state():
    summaries = sorted(_summaries.values(), key=lambda job: job.get("started_ns") or "", reverse=True)
    return wire.encode({"protocol": PROTOCOL, "monotonic": time.monotonic(), "state": STATE,
                        "stats": {"period": STATS._period, "gox": STATS._gox_cams},
                        "reference": {"status": fx10_reference.status(), "result": fx10_reference.last_result()},
                        "jobs": summaries})


def _arguments(function, payload: dict):
    args, kwargs = payload.get("args", []), payload.get("kwargs", {})
    if not isinstance(args, list) or not isinstance(kwargs, dict):
        raise ValueError("Invalid command arguments")
    bound = inspect.signature(function).bind(*args, **kwargs)
    hints = get_type_hints(function)
    # The fixed functions validate domain-specific details. Reject structural
    # surprises, non-finite values and overly large scalars before dispatch.
    for key, value in bound.arguments.items():
        if isinstance(value, (list, dict)) or isinstance(value, float) and not math.isfinite(value):
            raise ValueError(f"Invalid {key}")
        if isinstance(value, str) and len(value) > (MAX_BODY if key == "text" else 4096):
            raise ValueError(f"{key} exceeds its size limit")
        if key == "expected_path" and value is not None:
            if not isinstance(value, str):
                raise ValueError("expected_path must be a path string")
            bound.arguments[key] = Path(value)
            continue
        kind = hints.get(key)
        candidates = get_args(kind) if get_origin(kind) in (types.UnionType, Union) else (kind,)
        if not any((candidate is type(None) and value is None)
                   or (candidate is float and type(value) in (int, float))
                   or (candidate in (str, int, bool) and type(value) is candidate)
                   for candidate in candidates):
            raise ValueError(f"Incorrect type for {key}")
        if key == "exposure_ms" and not 0 < value <= 60000:
            raise ValueError("Exposure must be in (0, 60000] ms for GUI tools")
        if key == "gain" and not 0 <= value <= 10000:
            raise ValueError("Gain is outside the GUI tool limit")
    return bound


async def _execute(job: dict, function, bound) -> None:
    try:
        job["state"] = "running"
        _save(job)
        if job["action"] == "recording.stop" and job["session_generation"] != STATE.session_generation:
            raise RuntimeError("Recording changed before Stop was applied; review the current session")
        result = function(*bound.args, **bound.kwargs)
        if inspect.isawaitable(result):
            result = await result
        job.update(state="completed", result=wire.encode(result))
        _save(job)
    except BaseException as exc:
        job.pop("result", None)
        job.update(state="failed", error=str(exc) or "Command interrupted", error_type=type(exc).__name__)
        try:
            _save(job)
        except Exception:
            STATE.last_error = "Command journal could not be finalized; inspect controller state before retrying"


@api.post("/v1/jobs", status_code=202)
async def submit(request: Request):
    if not _accepting:
        raise HTTPException(503, "Controller is stopping")
    data = bytearray()
    async for chunk in request.stream():
        data.extend(chunk)
        if len(data) > MAX_BODY:
            raise HTTPException(413, "Request body exceeds the size limit")
    try:
        payload = json.loads(data)
        if not isinstance(payload, dict):
            raise ValueError("Expected a command object")
        job_id, action = payload["id"], payload["action"]
        path = _path(job_id)
        digest = hashlib.sha256(json.dumps(payload, sort_keys=True, allow_nan=False).encode()).hexdigest()
        if path.exists():
            previous = _read(job_id)
            if previous["digest"] != digest:
                raise HTTPException(409, "Request id already belongs to another command")
            return {"id": job_id, "state": previous["state"]}
        if payload.get("controller_id") != STATE.controller_id:
            raise HTTPException(409, "Controller changed; refresh state before issuing a command")
        function = _handlers()[action]
        bound = _arguments(function, payload)
    except (KeyError, ValueError, TypeError) as exc:
        raise HTTPException(400, str(exc)) from None
    if sum(not task.done() for task in _tasks.values()) >= 8:
        raise HTTPException(429, "Too many pending commands")
    stored = sorted(JOBS.glob("*.json"), key=lambda p: p.stat().st_mtime_ns)
    for old in stored:
        if len(stored) < MAX_JOBS:
            break
        if old.stem not in _tasks or _tasks[old.stem].done():
            old.unlink()
            stored = [p for p in stored if p != old]
            _tasks.pop(old.stem, None)
            _summaries.pop(old.stem, None)
    if len(stored) >= MAX_JOBS:
        raise HTTPException(429, "Command journal is full")
    job = {"id": job_id, "action": action, "digest": digest, "state": "accepted",
           "controller_id": STATE.controller_id, "session_generation": payload.get("session_generation"),
           "started_ns": str(time.time_ns())}
    _save(job)  # Persist receipt before any device operation can execute.
    _tasks[job_id] = asyncio.create_task(_execute(job, function, bound))
    return {"id": job_id, "state": "accepted"}


@api.get("/v1/jobs/{job_id}")
async def job(job_id: str):
    return _read(job_id)


def main() -> None:
    uvicorn.run(api, host=os.environ.get("AMIGA_CONTROLLER_HOST", "127.0.0.1"),
                port=int(os.environ.get("AMIGA_CONTROLLER_PORT", "8620")), workers=1,
                limit_concurrency=64, access_log=False)


if __name__ == "__main__":
    main()
