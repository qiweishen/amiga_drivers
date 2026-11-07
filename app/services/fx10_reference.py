"""Collect a persistent FX10 white/dark pair with the snapshot configuration.

Pairs are collected under <Output Directory>/tmp; the operator moves one into
the output directory with save_to_data() once the exposure check passes.
"""

from __future__ import annotations

import asyncio
import dataclasses
import json
import math
import time
import yaml
from datetime import datetime, timezone
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

from . import atomic_json, camera_operations, config_store, fx10_exposure, reference_storage, runtime, tool_jobs
from .fx10_reference_preview import ReferenceSpectrum, read_spectrum
from ..constants import BIN_FX10_REFERENCE, FX10_SNAPSHOT_CONFIG
from ..decoder import fx10
from ..decoder.ebus import recording as raw_recording

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
    exposure_check: fx10_exposure.ExposureCheck | None = None
    saved: bool = False  # moved from <output>/tmp into <output>


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
    path = config_store.checked_snapshot_path(FX10_SNAPSHOT_CONFIG)
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    if doc is None:
        doc = {}
    if not isinstance(doc, dict) or not isinstance(doc.get("reference", {}), dict):
        raise ValueError("FX10 config and reference must be mappings")
    reference = doc.get("reference", {})
    return path, _duration(reference["duration_s"]) if "duration_s" in reference else None


def status() -> str:
    return _status


def guard_reason() -> str | None:
    # Keep the existing whole-rig idle requirement for persistent reference capture.
    return camera_operations.guard_reason(None)


def _validate_result(emitted: str, output_root: Path, duration_s: float) -> tuple[Path, dict]:
    session = runtime.to_host_path(emitted).resolve()
    if session.parent != output_root.resolve() or not session.name.startswith("reference_"):
        raise ValueError("Reference result is outside the requested output directory")
    manifest = session / "reference.json"
    # Two verified signatures and wavelength/FWHM arrays can exceed 64 KiB.
    with manifest.open(encoding="utf-8") as source:
        text = source.read(4 * 1024 * 1024 + 1)
    if len(text) > 4 * 1024 * 1024:
        raise ValueError("Reference manifest is unexpectedly large")
    doc = json.loads(text)
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
        if not (path / "device.json").is_file():
            raise ValueError(f"{name.capitalize()} reference is missing finalized data")
        fx10.validate_finalized(path)
        ending = raw_recording.session_end(path)
        if (phase["frames_written"] != ending["valid_frames_written"]
                or phase.get("records_written") != ending["records_written"]):
            raise ValueError(f"{name.capitalize()} reference counters disagree with raw records")
    return session, doc


def _exposure_check(session: Path, manifest: dict, white: ReferenceSpectrum | None):
    """The white phase against the active exposure series; None if there is none or it does not apply."""
    if white is None or not white.ok or "p90" not in white.spectrum_dn:
        return None
    phase = next(phase for phase in manifest["phases"] if phase["phase"] == "white")
    white_dir = session / phase["session_dir"]
    return fx10_exposure.check_reference(white_dir, white.spectrum_dn["p90"], white.full_scale,
                                         f"{session.name}/{phase['session_dir']}")


def _move_to_output(session: Path) -> Path:
    root = reference_storage.output_root()
    if session.parent != root / reference_storage.TMP_FOLDER:
        raise ValueError(f"Only references under {root / reference_storage.TMP_FOLDER} can be saved")
    if (root / session.name).exists():
        raise ValueError(f"{root / session.name} already exists")
    # The collector's reference.json stays exactly as written; the move is recorded beside it.
    atomic_json.write(session / "saved.json", {
        "format": "fx10-reference-saved-v1", "saved_utc": datetime.now(timezone.utc).isoformat(),
        "saved_from": session.relative_to(root).as_posix(),
    }, limit=64 * 1024)
    return reference_storage.move_to_output(session, root)


async def save_to_data() -> ReferenceResult:
    """Move the last completed pair from <output>/tmp into <output>."""
    global _last_result, _status
    result = _last_result
    if result is None or not result.ok or not result.session_dir:
        raise ValueError("There is no completed reference pair to save")
    if result.saved:
        raise ValueError(f"This reference pair is already saved: {result.session_dir}")
    target = await asyncio.to_thread(_move_to_output, Path(result.session_dir))
    _last_result = dataclasses.replace(result, session_dir=str(target), saved=True)
    _status = f"FX10 reference pair saved: {target}"
    return _last_result


async def _terminate(proc: asyncio.subprocess.Process) -> None:
    await tool_jobs.terminate(proc)


async def collect_reference() -> ReferenceResult:
    return await camera_operations.run(None, _collect, kind="fx10-reference")


async def _collect() -> ReferenceResult:
    global _status, _last_result
    started = time.monotonic()
    _status = "Preparing reference capture using the FX10 snapshot configuration…"
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
        config = config_store.checked_snapshot_path(FX10_SNAPSHOT_CONFIG)
        output_root = await asyncio.to_thread(reference_storage.tmp_root)
        # No preview slider values or selected-camera overrides. The C++ tool
        # reads this config once and preserves that exact text alongside the pair.
        # Reference capture always uses freerun; it does not use SensorSync.
        argv = [
            runtime.exec_path(BIN_FX10_REFERENCE), "--config", runtime.exec_path(config),
            "--out", runtime.exec_path(output_root),
        ]
        proc = await tool_jobs.spawn(argv)

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
        await tool_jobs.confirm_exit(proc)
        if proc.returncode != 0 or failure or not success_path:
            raise RuntimeError(
                failure or f"Reference collector exited without a complete pair (exit {proc.returncode})")
        if duration_s is None:
            raise RuntimeError("Collector did not announce the configured duration; rebuild fx10_reference")
        session, manifest = await asyncio.to_thread(_validate_result, success_path, output_root, duration_s)
        spectra = {}
        for phase in manifest["phases"]:
            name = phase["phase"]
            _status = f"References saved — calculating the complete {name} spectrum…"
            try:
                spectrum = await asyncio.to_thread(read_spectrum, session / phase["session_dir"],
                                                   phase["frames_written"])
            except Exception as error:
                # Recording already completed. A display/analysis error must
                # not reclassify the saved pair as a failed acquisition.
                spectrum = ReferenceSpectrum(False, reason=f"Cannot display reference spectrum: {error}")
            spectra[name] = spectrum
            _last_result = ReferenceResult(True, session_dir=str(session), duration_s=duration_s,
                                           spectra=dict(spectra), elapsed_s=time.monotonic() - started,
                                           raw_output="\n".join(output))
        try:
            check = await asyncio.to_thread(_exposure_check, session, manifest, spectra.get("white"))
        except Exception:  # an analysis error never reclassifies the recorded pair
            check = None
        if check is not None:
            await fx10_exposure.save_current()
            _last_result = dataclasses.replace(_last_result, exposure_check=check)
        _status = f"White and dark references collected under tmp: {session}"
        if any(not spectrum.ok for spectrum in spectra.values()):
            _status += " — some plots are unavailable; recorded data are retained"
        return _last_result
    except asyncio.CancelledError:
        _status = "Reference collection interrupted; any partial data are retained"
        raise
    except Exception as error:
        reason = f"Reference collection timed out after {timeout_s:g}s" if isinstance(error,
                                                                                      asyncio.TimeoutError) else str(
            error)
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
