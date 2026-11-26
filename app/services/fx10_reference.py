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
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

from . import atomic_json, camera_operations, config_store, fx10_exposure, reference_storage, runtime, tool_jobs
from ..constants import BIN_FX10_REFERENCE, BIN_PHENOMATE, FX10_SNAPSHOT_CONFIG
from ..decoder import fx10
from ..decoder.ebus import recording as raw_recording

SETUP_AND_FINALIZE_TIMEOUT_S = 180.0  # added to BOTH configured capture intervals
_status = ""


@dataclass(frozen=True)
class ReferenceSpectrum:
    """Whole-phase statistics for the page; a failure here never changes the raw data."""
    ok: bool
    reason: str = ""
    wavelengths_nm: list[float] = field(default_factory=list)
    spectrum_dn: dict[str, list[float]] = field(default_factory=dict)
    full_scale: int = 0
    pixel_format: str = ""
    frames: int = 0
    bands: int = 0
    samples: int = 0
    mean_dn: float = 0.0
    clipped_pct: float = 0.0


def read_spectrum(session_dir: Path, expected_frames: int) -> ReferenceSpectrum:
    try:
        capture, count, values, mean, clipped = fx10.reference_spectrum(session_dir, expected_frames)
        return ReferenceSpectrum(True, wavelengths_nm=list(capture.wavelengths_nm), spectrum_dn=values,
                                 full_scale=capture.full_scale, pixel_format=capture.pixel_format,
                                 frames=count, bands=capture.bands, samples=capture.samples,
                                 mean_dn=mean, clipped_pct=clipped)
    except Exception as error:  # display only: never fails the recorded pair
        return ReferenceSpectrum(False, reason=f"Cannot display reference spectrum: {error}")


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


def _validate_result(emitted: str, output_root: Path, duration_s: float) -> tuple[Path, dict]:
    session = Path(emitted).resolve()
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


def _record_save(session: Path, note: dict) -> None:
    # The collector's reference.json stays exactly as written; the move is recorded beside it.
    atomic_json.write(session / "saved.json", {"format": "fx10-reference-saved-v1", **note})


async def save_to_data() -> ReferenceResult:
    """Move the last completed pair from <output>/tmp into <output>."""
    global _last_result, _status
    result = _last_result
    if result is None or not result.ok or not result.session_dir:
        raise ValueError("There is no completed reference pair to save")
    if result.saved:
        raise ValueError(f"This reference pair is already saved: {result.session_dir}")
    target = await asyncio.to_thread(reference_storage.save, Path(result.session_dir), _record_save)
    _last_result = dataclasses.replace(result, session_dir=str(target), saved=True)
    _status = f"FX10 reference pair saved: {target}"
    return _last_result


async def collect_reference() -> ReferenceResult:
    return await camera_operations.run(None, _collect, kind="fx10-reference")


@dataclass
class _Progress:
    """The collector's stdout markers (fx10_driver/tools/reference_main.cpp; text kept verbatim)."""
    output: deque = field(default_factory=lambda: deque(maxlen=400))
    configured: asyncio.Event = field(default_factory=asyncio.Event)
    duration_s: float | None = None
    session: str = ""  # REFERENCE: SESSION, also for a failed collection's partial data
    success: str = ""  # REFERENCE: OK
    failure: str = ""  # REFERENCE: FAIL

    def duration(self) -> float:
        if self.duration_s is None:
            raise ValueError("Collector did not announce the configured duration; rebuild fx10_reference")
        return self.duration_s

    def feed(self, text: str) -> None:
        global _status
        self.output.append(text)
        if text.startswith("REFERENCE: CONFIG "):
            if self.configured.is_set():
                raise ValueError("Reference collector announced its configuration twice")
            self.duration_s = _duration(json.loads(text.removeprefix("REFERENCE: CONFIG "))["duration_s"])
            _status = f"Preparing camera — {self.duration_s:g} seconds per reference…"
            self.configured.set()
        elif text.startswith("REFERENCE: SESSION "):
            self.session = text.removeprefix("REFERENCE: SESSION ")
        elif text in ("REFERENCE: PHASE white", "REFERENCE: PHASE dark"):
            _status = f"Collecting {text.split()[-1]} reference — {self.duration():g} seconds…"
        elif text.startswith("REFERENCE: OK "):
            self.success = text.removeprefix("REFERENCE: OK ")
        elif text.startswith("REFERENCE: FAIL "):
            self.failure = text.removeprefix("REFERENCE: FAIL ")


async def _wait(proc: asyncio.subprocess.Process, progress: _Progress) -> None:
    """Read the collector until it exits. Setup and finalization get SETUP_AND_FINALIZE_TIMEOUT_S,
    plus both capture intervals once the collector has announced its duration."""
    async def read(stream, handle) -> None:
        while line := await stream.readline():
            handle(line.decode(errors="replace").rstrip())

    started = time.monotonic()
    timeout_s = SETUP_AND_FINALIZE_TIMEOUT_S
    completed = asyncio.gather(read(proc.stdout, progress.feed), read(proc.stderr, progress.output.append),
                               proc.wait())
    configured = asyncio.create_task(progress.configured.wait())
    try:
        done, _ = await asyncio.wait((completed, configured), timeout=timeout_s,
                                     return_when=asyncio.FIRST_COMPLETED)
        if done:
            timeout_s += 2 * (progress.duration_s or 0.0)
            await asyncio.wait_for(asyncio.shield(completed), max(0.1, timeout_s - (time.monotonic() - started)))
    except asyncio.TimeoutError:
        done = set()
    except BaseException:
        await _stop(proc, completed)
        raise
    finally:
        configured.cancel()
        await asyncio.gather(configured, return_exceptions=True)
    if not done:
        await _stop(proc, completed)
        raise RuntimeError(f"Reference collection timed out after {timeout_s:g}s")


async def _stop(proc: asyncio.subprocess.Process, completed: asyncio.Future) -> None:
    await tool_jobs.terminate(proc)
    # Bounded drain: a broken client must not hold the camera reservation after the collector ended.
    await asyncio.wait([completed], timeout=5)
    completed.cancel()
    await asyncio.gather(completed, return_exceptions=True)


async def _spectra(session: Path, manifest: dict, progress: _Progress, started: float) -> dict:
    """Whole-phase spectra, published one by one; an analysis error never fails the recorded pair."""
    global _status, _last_result
    spectra = {}
    for phase in manifest["phases"]:
        _status = f"References saved — calculating the complete {phase['phase']} spectrum…"
        spectra[phase["phase"]] = await asyncio.to_thread(read_spectrum, session / phase["session_dir"],
                                                          phase["frames_written"])
        _last_result = ReferenceResult(True, session_dir=str(session), duration_s=progress.duration_s,
                                       spectra=dict(spectra), elapsed_s=time.monotonic() - started,
                                       raw_output="\n".join(progress.output))
    return spectra


async def _collect() -> ReferenceResult:
    global _status, _last_result
    started = time.monotonic()
    _status = "Preparing reference capture using the FX10 snapshot configuration…"
    _last_result = None
    progress = _Progress()
    try:
        # camera_operations already excludes this GUI's recording; this also catches one started elsewhere.
        if await runtime.pgrep(BIN_PHENOMATE.name):
            raise RuntimeError("Stop the main recording before collecting references")
        config = config_store.checked_snapshot_path(FX10_SNAPSHOT_CONFIG)
        output_root = await asyncio.to_thread(reference_storage.tmp_root)
        # The tool reads this config once (freerun, no SensorSync) and keeps its exact text with the pair.
        proc = await tool_jobs.spawn([str(BIN_FX10_REFERENCE), "--config", str(config), "--out", str(output_root)])
        await _wait(proc, progress)
        await tool_jobs.confirm_exit(proc)
        if proc.returncode != 0 or progress.failure or not progress.success:
            raise RuntimeError(progress.failure
                               or f"Reference collector exited without a complete pair (exit {proc.returncode})")
        session, manifest = await asyncio.to_thread(_validate_result, progress.success, output_root,
                                                    progress.duration())
        spectra = await _spectra(session, manifest, progress, started)
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
        _status = f"Reference collection failed: {error}"
        if progress.session:
            _status += f" — partial data retained at {progress.session}"
        _last_result = ReferenceResult(False, reason=str(error), session_dir=progress.session,
                                       duration_s=progress.duration_s, elapsed_s=time.monotonic() - started,
                                       raw_output="\n".join(progress.output))
        return _last_result
