"""Collect a persistent FX10 white/dark pair with the saved driver configuration."""

from __future__ import annotations

import asyncio
import json
import math
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

import yaml

from ..constants import BIN_FX10_REFERENCE
from ..state import STATE
from . import camera_operations, config_store, runtime
from .fx10_reference_preview import ReferenceSpectrum, read_spectrum

TOOL_NAME = "fx10_reference"
SETUP_AND_FINALIZE_TIMEOUT_S = 180.0  # added to BOTH configured capture intervals
_status = ""


@dataclass(frozen=True)
class ReferenceResult:
    ok: bool
    reason: str = ""
    session_dir: str = ""
    elapsed_s: float = 0.0
    raw_output: str = ""
    duration_s: float | None = None
    spectra: dict[str, ReferenceSpectrum] = field(default_factory=dict)


_last_result: ReferenceResult | None = None


def last_result() -> ReferenceResult | None:
    """Latest operation in this GUI process, retained across page navigation."""
    return _last_result


def _duration(value: object) -> float:
    if isinstance(value, bool):
        raise ValueError("reference.duration_s must be numeric")
    duration = float(value)
    if not math.isfinite(duration) or not 0.1 <= duration <= 3600:
        raise ValueError("reference.duration_s must be finite and within [0.1, 3600] seconds")
    return duration


def configured_duration() -> tuple[Path, float | None]:
    """For page display only; the collector announces its actual loaded value."""
    loaded = config_store.read("fx10")
    doc = yaml.safe_load(loaded.text)
    if doc is None:
        doc = {}
    if not isinstance(doc, dict) or not isinstance(doc.get("reference", {}), dict):
        raise ValueError("FX10 config and reference must be mappings")
    reference = doc.get("reference", {})
    return loaded.file.path, _duration(reference["duration_s"]) if "duration_s" in reference else None


def status() -> str:
    return _status


def guard_reason() -> str | None:
    # Reference collection can use the shared trigger controller, even when the
    # main recording has FX10 disabled. It needs the whole rig to be idle.
    return camera_operations.guard_reason(None)


def _validate_result(emitted: str, output_root: Path, duration_s: float) -> tuple[Path, dict]:
    session = runtime.to_host_path(emitted).resolve()
    if session.parent != output_root.resolve() or not session.name.startswith("reference_"):
        raise ValueError("Reference result is outside the requested output directory")
    manifest = session / "reference.json"
    if manifest.stat().st_size > 65536:
        raise ValueError("Reference manifest is unexpectedly large")
    doc = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(doc, dict):
        raise ValueError("Invalid reference manifest")
    phases = doc.get("phases", [])
    if (doc.get("format") != "fx10-reference-v1" or doc.get("status") != "completed"
            or _duration(doc.get("phase_duration_s")) != duration_s
            or not isinstance(phases, list) or len(phases) != 2):
        raise ValueError("Reference pair is incomplete")
    for name, phase in zip(("white", "dark"), phases):
        if (not isinstance(phase, dict) or phase.get("phase") != name or phase.get("status") != "completed"
                or _duration(phase.get("requested_duration_s")) != duration_s
                or type(phase.get("frames_written")) is not int or phase["frames_written"] <= 0):
            raise ValueError(f"{name.capitalize()} reference did not complete")
        path = (session / phase["session_dir"]).resolve()
        if path.parent != session / "raw" / "fx10" / name:
            raise ValueError("Reference phase path does not match its label")
        headers = list(path.glob("segment_*.hdr"))
        if not (path / "capture.json").is_file() or not (path / "device.json").is_file() or not headers:
            raise ValueError(f"{name.capitalize()} reference is missing finalized data")
        for header in headers:
            for suffix in (".bil", ".lines.csv"):
                data = header.with_suffix(suffix)
                if not data.is_file() or data.stat().st_size == 0:
                    raise ValueError(f"{name.capitalize()} reference is missing {data.name}")
    return session, doc


async def _terminate(proc: asyncio.subprocess.Process) -> None:
    """Stop the tool, including its actual container process, then reap the client."""
    try:
        if runtime.is_docker():
            identity = await runtime.running_process(TOOL_NAME)
            if identity is not None:
                response = await runtime.signal_process(identity)
                if not response.ok and await runtime.is_process_alive(identity):
                    raise RuntimeError("Cannot terminate reference collector")
                deadline = time.monotonic() + 15
                while await runtime.is_process_alive(identity) and time.monotonic() < deadline:
                    await asyncio.sleep(0.1)
                if await runtime.is_process_alive(identity):
                    response = await runtime.exec_(["kill", "-KILL", "--", str(identity.pid)], timeout=5)
                    if not response.ok:
                        raise RuntimeError("Cannot kill stalled reference collector")
                    deadline = time.monotonic() + 5
                    while await runtime.is_process_alive(identity) and time.monotonic() < deadline:
                        await asyncio.sleep(0.1)
                    if await runtime.is_process_alive(identity):
                        raise RuntimeError("Reference collector is still running")
        elif proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=15)
            except asyncio.TimeoutError:
                proc.kill()
        if proc.returncode is None:
            # The remote child is known to have ended; reap any remaining exec client.
            if runtime.is_docker():
                proc.kill()
            await asyncio.wait_for(proc.wait(), timeout=5)
    except ProcessLookupError:
        try:
            await asyncio.wait_for(proc.wait(), timeout=5)
        except Exception:
            STATE.control_uncertain = True
            raise
    except Exception:
        STATE.control_uncertain = True
        raise


async def collect_reference() -> ReferenceResult:
    return await camera_operations.run(None, _collect)


async def _collect() -> ReferenceResult:
    global _status, _last_result
    started = time.monotonic()
    _status = "Preparing reference capture using the saved FX10 configuration…"
    _last_result = None
    proc = None
    completed = None
    ready_wait = None
    configured = asyncio.Event()
    duration_s = None
    timeout_s = SETUP_AND_FINALIZE_TIMEOUT_S
    readers: list[asyncio.Task] = []
    output: deque[str] = deque(maxlen=400)
    success_path = ""
    failure = ""
    session_path = ""
    try:
        if not await runtime.binary_exists(BIN_FX10_REFERENCE):
            raise RuntimeError("build/bin/fx10_reference is not available in the execution environment")
        if await runtime.pgrep(TOOL_NAME):
            raise RuntimeError("A reference collector is already running")
        if await runtime.pgrep("AmigaDrivers"):
            raise RuntimeError("Stop the main recording before collecting references")
        config = config_store.get("fx10")
        output_root = config_store.main_settings()["output_dir"]
        if output_root is None:
            raise ValueError("The configured output directory is outside the shared mounts")
        # No preview slider values or selected-camera overrides. The C++ tool
        # reads this config once and preserves that exact text alongside the pair.
        proc = await runtime.popen([
            runtime.exec_path(BIN_FX10_REFERENCE), "--config", runtime.exec_path(config.path),
            "--out", runtime.exec_path(output_root),
        ])

        async def read_stdout() -> None:
            nonlocal success_path, failure, session_path, duration_s
            global _status
            while line := await proc.stdout.readline():
                text = line.decode(errors="replace").rstrip()
                output.append(text)
                if text.startswith("REFERENCE: CONFIG "):
                    doc = json.loads(text.removeprefix("REFERENCE: CONFIG "))
                    if configured.is_set():
                        raise ValueError("Reference collector announced its configuration twice")
                    duration_s = _duration(doc["duration_s"])
                    _status = f"Preparing camera — {duration_s:g} seconds per reference…"
                    configured.set()
                elif text.startswith("REFERENCE: SESSION "):
                    session_path = str(runtime.to_host_path(text.removeprefix("REFERENCE: SESSION ")))
                elif text == "REFERENCE: PHASE white":
                    if duration_s is None:
                        raise ValueError("Collector did not announce the configured duration; rebuild fx10_reference")
                    _status = f"Collecting white reference — {duration_s:g} seconds…"
                elif text == "REFERENCE: PHASE dark":
                    if duration_s is None:
                        raise ValueError("Collector did not announce the configured duration; rebuild fx10_reference")
                    _status = f"Collecting dark reference — {duration_s:g} seconds…"
                elif text.startswith("REFERENCE: OK "):
                    success_path = text.removeprefix("REFERENCE: OK ")
                elif text.startswith("REFERENCE: FAIL "):
                    failure = text.removeprefix("REFERENCE: FAIL ")

        async def read_stderr() -> None:
            while line := await proc.stderr.readline():
                output.append(line.decode(errors="replace").rstrip())

        readers = [asyncio.create_task(read_stdout()), asyncio.create_task(read_stderr())]
        # Shield readers so timeout handling can drain output during graceful shutdown.
        completed = asyncio.gather(*readers, proc.wait())
        try:
            ready_wait = asyncio.create_task(configured.wait())
            done, _ = await asyncio.wait((completed, ready_wait), timeout=timeout_s,
                                         return_when=asyncio.FIRST_COMPLETED)
            if not done:
                raise asyncio.TimeoutError()
            if duration_s is not None:
                timeout_s += 2 * duration_s
            await asyncio.wait_for(asyncio.shield(completed),
                                   timeout=max(0.1, timeout_s - (time.monotonic() - started)))
        except BaseException:
            await _terminate(proc)
            # Bound pipe draining too; a broken exec client must not hold the
            # camera reservation indefinitely after the collector has ended.
            try:
                await asyncio.wait_for(asyncio.shield(completed), timeout=5)
            except Exception:
                pass
            raise
        if proc.returncode != 0 or failure or not success_path:
            raise RuntimeError(failure or f"Reference collector exited without a complete pair (exit {proc.returncode})")
        if duration_s is None:
            raise RuntimeError("Collector did not announce the configured duration; rebuild fx10_reference")
        session, manifest = await asyncio.to_thread(_validate_result, success_path, output_root, duration_s)
        spectra = {}
        for phase in manifest["phases"]:
            name = phase["phase"]
            _status = f"References saved — calculating the complete {name} spectrum…"
            try:
                spectrum = await asyncio.to_thread(read_spectrum, session / phase["session_dir"], phase["frames_written"])
            except Exception as error:
                # Recording already completed. A display/analysis error must
                # not reclassify the saved pair as a failed acquisition.
                spectrum = ReferenceSpectrum(False, reason=f"Cannot display reference spectrum: {error}")
            spectra[name] = spectrum
            _last_result = ReferenceResult(True, session_dir=str(session), duration_s=duration_s,
                spectra=dict(spectra), elapsed_s=time.monotonic() - started, raw_output="\n".join(output))
        _status = f"White and dark references saved: {session}"
        if any(not spectrum.ok for spectrum in spectra.values()):
            _status += " — some plots are unavailable; recorded data are retained"
        return _last_result
    except asyncio.CancelledError:
        _status = "Reference collection interrupted; any partial data are retained"
        raise
    except Exception as error:
        reason = f"Reference collection timed out after {timeout_s:g}s" if isinstance(error, asyncio.TimeoutError) else str(error)
        _status = f"Reference collection failed: {reason}"
        if session_path:
            _status += f" — partial data retained at {session_path}"
        _last_result = ReferenceResult(False, reason=reason, session_dir=session_path, duration_s=duration_s,
                                      elapsed_s=time.monotonic() - started, raw_output="\n".join(output))
        return _last_result
    finally:
        if ready_wait is not None:
            if not ready_wait.done():
                ready_wait.cancel()
            await asyncio.gather(ready_wait, return_exceptions=True)
        if completed is not None:
            if not completed.done():
                completed.cancel()
            await asyncio.gather(completed, return_exceptions=True)
        for reader in readers:
            if not reader.done():
                reader.cancel()
        if readers:
            await asyncio.gather(*readers, return_exceptions=True)
