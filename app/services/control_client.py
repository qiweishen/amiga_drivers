"""Server-side GUI client. The browser never receives the controller credential."""

from __future__ import annotations

import asyncio
import dataclasses
import json
import os
import time
import uuid
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request, build_opener, ProxyHandler, HTTPRedirectHandler

from ..state import STATE, AppState
from . import control_token, wire

PROTOCOL = "amiga-control-v1"
_polling = False
_tasks: set[asyncio.Task] = set()
_slots = asyncio.Semaphore(4)
_reference = None


def enabled() -> bool:
    return bool(os.environ.get("AMIGA_CONTROLLER_URL", "").strip())


def token() -> str:
    return control_token.read()


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise RuntimeError("Controller redirects are not accepted")


def _request(method: str, path: str, payload=None):
    base = os.environ["AMIGA_CONTROLLER_URL"].rstrip("/")
    url = urlsplit(base)
    if url.scheme not in ("http", "https") or not url.hostname or url.username or url.password or url.query or url.fragment:
        raise ValueError("Invalid controller URL")
    data = json.dumps(wire.encode(payload), allow_nan=False).encode() if payload is not None else None
    request = Request(base + path, data=data, method=method,
                      headers={"Authorization": "Bearer " + token(), "Content-Type": "application/json"})
    opener = build_opener(ProxyHandler({}), _NoRedirect())
    try:
        response = opener.open(request, timeout=10)
    except HTTPError as error:
        with error:
            detail = error.read(4096).decode(errors="replace")
        raise RuntimeError(f"Controller rejected request ({error.code}): {detail}") from None
    with response:
        raw = response.read(16 * 1024 * 1024 + 1)
    if len(raw) > 16 * 1024 * 1024:
        raise ValueError("Controller response exceeds the size limit")
    return json.loads(raw)


async def request(method: str, path: str, payload=None):
    async with _slots:
        return await asyncio.to_thread(_request, method, path, payload)


async def call(action: str, *args, **kwargs):
    """Submit once, then poll by id. Network errors never replay a hardware command."""
    if not STATE.controller_connected:
        raise RuntimeError("Wait for the controller connection before issuing a command")
    context = STATE.controller_id, STATE.session_generation
    async def execute():
        job_id = uuid.uuid4().hex
        payload = {"id": job_id, "action": action, "args": list(args), "kwargs": kwargs,
                   "controller_id": context[0], "session_generation": context[1]}
        try:
            await request("POST", "/v1/jobs", payload)
        except Exception as exc:
            STATE.env_ok = STATE.controller_connected = False
            raise RuntimeError(f"Command receipt is unknown (job {job_id}); inspect controller state before retrying: {exc}") from exc
        while True:
            try:
                job = await request("GET", f"/v1/jobs/{job_id}")
            except Exception as exc:
                STATE.env_ok = STATE.controller_connected = False
                raise RuntimeError(f"Job {job_id} may still be running; reconnect and inspect its state: {exc}") from exc
            if job["state"] == "failed":
                from . import config_store
                error_cls = {"ConflictError": config_store.ConflictError,
                             "ConfigPathChangedError": config_store.ConfigPathChangedError}.get(job.get("error_type"), RuntimeError)
                raise error_cls(job["error"])
            if job["state"] == "completed":
                await poll()
                return job.get("result")
            await asyncio.sleep(0.5)

    task = asyncio.create_task(execute())
    _tasks.add(task)
    def finished(done):
        _tasks.discard(done)
        if not done.cancelled():
            done.exception()
    task.add_done_callback(finished)
    return await asyncio.shield(task)


async def poll() -> None:
    global _polling, _reference
    if _polling:
        return
    _polling = True
    try:
        started = time.monotonic()
        doc = await request("GET", "/v1/state")
        if doc.get("protocol") != PROTOCOL:
            raise ValueError("Controller protocol mismatch")
        state = wire.restore(AppState, doc["state"])
        offset = started - float(doc["monotonic"])
        for sensor in state.sensors.values():
            if sensor.write_fps_at:
                sensor.write_fps_at += offset
        if state.storage.sampled_at:
            state.storage.sampled_at += offset
        state.mode = "remote"
        state.controller_connected = True
        state.recent_commands = doc.get("jobs", [])
        for field in dataclasses.fields(AppState):
            setattr(STATE, field.name, getattr(state, field.name))
        from . import fx10_reference
        from .driver_stats import STATS
        STATS._period = doc["stats"]["period"]
        STATS._gox_cams = {key: (value[0], value[1] + offset) for key, value in doc["stats"]["gox"].items()}
        fx10_reference._status = doc["reference"]["status"]
        if doc["reference"].get("result") != _reference:
            _reference = doc["reference"].get("result")
            fx10_reference._last_result = wire.restore(fx10_reference.ReferenceResult, _reference) if _reference else None
    except Exception as exc:
        STATE.controller_connected = False
        STATE.env_ok = False
        STATE.env_detail = f"Controller unavailable: {exc}"
    finally:
        _polling = False
