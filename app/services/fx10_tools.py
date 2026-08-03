"""FX10 helpers: eBUS device discovery (fx10_snapshot --list) and the snapshot
pipeline (fx10_snapshot in the container -> ENVI BIL decode on the host ->
per-band spectral statistics over ~1 s of frames, normalized to 100%)."""

from __future__ import annotations

import asyncio
import json
import re
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import yaml

from ..constants import (
    BIN_FX10_SNAPSHOT,
    CONFIG_FILES,
    FX10_SNAPSHOT_DIR,
    SNAPSHOT_KEEP,
)
from ..state import STATE, ProcState
from . import runtime

SNAPSHOT_TOOL = "fx10_snapshot"
FX10_CONFIG = CONFIG_FILES["fx10"].path
# Freerun rate forced inside the tool: min(its --fps default, 1000/exposure_ms)
# — long exposures lower the achievable rate. The GUI-side hard timeout must
# stay strictly greater than the in-tool wall-clock budget
# (frames / effective_fps * 3 + 5 s) so the tool gets to fail first.
SNAPSHOT_FPS = 50.0


def _timeout_s(frames: int, exposure_ms: float) -> float:
    effective_fps = min(SNAPSHOT_FPS, 1000.0 / max(exposure_ms, 0.01))
    return frames / effective_fps * 3 + 10


@dataclass
class Device:
    display_id: str
    connection_id: str
    ip: str
    mac: str


@dataclass
class DiscoverResult:
    devices: list[Device] = field(default_factory=list)
    raw_output: str = ""
    error: str = ""


@dataclass
class SnapshotResult:
    ok: bool
    reason: str = ""  # FAIL reason / pipeline error
    wavelengths_nm: list[float] = field(default_factory=list)  # x axis, one per band
    # Per-band statistics over ALL captured frames and samples, as % of full scale
    spectrum_pct: dict[str, list[float]] = field(default_factory=dict)
    clipped_pct: float = 0.0  # saturated pixels over the FULL cube
    mean_pct: float = 0.0  # cube mean as % of full scale
    lines: int = 0
    bands: int = 0
    samples: int = 0
    elapsed_s: float = 0.0
    raw_output: str = ""
    session_dir: str = ""


def guard_reason() -> str | None:
    """Why snapshot/discover must NOT run right now (GigE control is exclusive).

    Uses the Enable-FX10 value captured at process start — the live file value
    can be toggled mid-run and must not unlock the camera the driver owns.
    """
    if STATE.process_state in (ProcState.RUNNING, ProcState.STARTING, ProcState.STOPPING):
        if STATE.enables_at_start.get("fx10", False):
            return "Recording is running with FX10 enabled — the driver owns the camera; stop recording first"
    return None


async def discover(timeout_ms: int = 1500) -> DiscoverResult:
    snapshot_bin = runtime.exec_path(BIN_FX10_SNAPSHOT)
    res = await runtime.exec_(
        [snapshot_bin, "--list", "--timeout-ms", str(timeout_ms)], timeout=timeout_ms / 1000 + 15
    )
    raw = (res.stdout + ("\n" + res.stderr if res.stderr.strip() else "")).strip()
    # The implicit spdlog stdout logger may precede the JSON: parse the LAST
    # line that looks like a JSON object.
    line = next((ln for ln in reversed(res.stdout.splitlines()) if ln.lstrip().startswith("{")), None)
    if line is None:
        return DiscoverResult(
            raw_output=raw,
            error=f"fx10_snapshot --list produced no JSON (exit {res.code}): {res.stderr.strip()}",
        )
    try:
        doc = json.loads(line)
    except json.JSONDecodeError as e:
        return DiscoverResult(raw_output=raw, error=f"Cannot parse the --list output: {e}")
    if "error" in doc:
        return DiscoverResult(raw_output=raw, error=f"fx10_snapshot --list failed: {doc['error']}")
    devices = [
        Device(
            display_id=d.get("display_id", ""),
            connection_id=d.get("connection_id", ""),
            ip=d.get("ip", ""),
            mac=d.get("mac", ""),
        )
        for d in doc.get("devices", [])
    ]
    return DiscoverResult(devices=devices, raw_output=raw)


async def snapshot(ip: str, exposure_ms: float) -> SnapshotResult:
    """One spectral preview: capture ~1 second of frames and reduce them to
    per-band statistics. Caller must have checked guard_reason() and must
    serialize calls (STATE.snapshot_busy)."""
    t0 = time.monotonic()
    # All frames of ONE second at the achievable rate (the tool caps its fps
    # the same way, so this is exactly the frames it can deliver in 1 s).
    frames = max(1, round(min(SNAPSHOT_FPS, 1000.0 / max(exposure_ms, 0.01))))
    timeout_s = _timeout_s(frames, exposure_ms)
    sid = time.strftime("%Y%m%d_%H%M%S")
    out_host = FX10_SNAPSHOT_DIR / sid
    if out_host.exists():  # same-second collision on rapid consecutive shots
        sid = f"{sid}_{int((time.time() % 1) * 1000):03d}"
        out_host = FX10_SNAPSHOT_DIR / sid
    proc = await runtime.popen([
        runtime.exec_path(BIN_FX10_SNAPSHOT),
        "--config", runtime.exec_path(FX10_CONFIG),
        "--out", runtime.exec_path(out_host),
        "--ip", ip,
        "--frames", str(frames),
        "--exposure-ms", str(exposure_ms),
    ])
    try:
        out_b, err_b = await asyncio.wait_for(proc.communicate(), timeout=timeout_s)
    except asyncio.TimeoutError:
        # In docker mode killing the exec client does not touch the remote
        # tool — a hung fx10_snapshot would hold the camera's control channel
        # forever; pkill works identically on both backends.
        await runtime.pkill(SNAPSHOT_TOOL, "TERM")
        await asyncio.sleep(3)
        if await runtime.pgrep(SNAPSHOT_TOOL):
            await runtime.pkill(SNAPSHOT_TOOL, "KILL")
        proc.kill()
        await proc.wait()
        return SnapshotResult(False, reason=f"Timed out (>{timeout_s:.0f}s) — fx10_snapshot was terminated",
                              elapsed_s=time.monotonic() - t0)

    stdout = out_b.decode(errors="replace")
    stderr = err_b.decode(errors="replace")
    raw_output = (stdout + "\n--- stderr ---\n" + stderr).strip()

    marker = next((ln for ln in reversed(stdout.splitlines()) if ln.startswith("SNAPSHOT: ")), None)
    if marker is None:
        return SnapshotResult(False, reason=f"No SNAPSHOT marker in the output (exit {proc.returncode})",
                              raw_output=raw_output, elapsed_s=time.monotonic() - t0)
    if marker.startswith("SNAPSHOT: FAIL"):
        reason = marker[len("SNAPSHOT: FAIL "):]
        if reason.strip().startswith("3"):
            reason += " (connect/control failure — see the raw output for the failing node)"
        return SnapshotResult(False, reason=reason, raw_output=raw_output,
                              elapsed_s=time.monotonic() - t0)

    session_dir_emitted = marker[len("SNAPSHOT: OK "):].strip()
    try:
        session_dir = runtime.to_host_path(session_dir_emitted)
    except ValueError as e:
        return SnapshotResult(False, reason=str(e), raw_output=raw_output,
                              elapsed_s=time.monotonic() - t0)

    # Decode on the host (venv has numpy): ENVI BIL -> per-band statistics,
    # normalized against the configured pixel format's ADC full scale.
    result = await asyncio.to_thread(_decode_envi, session_dir, _configured_full_scale())
    result.raw_output = raw_output
    result.elapsed_s = time.monotonic() - t0
    if result.reason:
        return result
    result.ok = True
    result.session_dir = str(session_dir)

    await asyncio.to_thread(_cleanup_old)
    return result


def _hdr_int(text: str, key: str) -> int | None:
    m = re.search(rf"^{key}\s*=\s*(\d+)\s*$", text, re.MULTILINE | re.IGNORECASE)
    return int(m.group(1)) if m else None


# ADC full scale implied by each supported pixel format (10-bit formats are
# stored in the same canonical uint16 as 12-bit ones, so the ENVI data type
# alone cannot distinguish them).
_PIXEL_FULL_SCALE = {
    "Mono8": 255,
    "Mono10": 1023,
    "Mono10Packed": 1023,
    "Mono12": 4095,
    "Mono12Packed": 4095,
}


def _configured_full_scale() -> int | None:
    """Full scale from acquisition.pixel_format in config-fx10.yaml — the same
    file the snapshot tool captures with, so it matches the data on disk."""
    try:
        doc = yaml.safe_load(FX10_CONFIG.read_text(encoding="utf-8")) or {}
        pixel_format = str((doc.get("acquisition") or {}).get("pixel_format", ""))
        return _PIXEL_FULL_SCALE.get(pixel_format)
    except Exception:
        return None


def _hdr_wavelengths(text: str, bands: int) -> list[float]:
    """Wavelength axis from the .hdr `wavelength = { ... }` block; falls back
    to the FX10e's nominal linear 400..1000 nm grid when absent."""
    m = re.search(r"^wavelength\s*=\s*\{([^}]*)\}", text, re.MULTILINE | re.IGNORECASE | re.DOTALL)
    if m:
        try:
            values = [float(v) for v in m.group(1).replace("\n", " ").split(",") if v.strip()]
            if len(values) == bands:
                return values
        except ValueError:
            pass
    return list(np.linspace(400.0, 1000.0, bands))


def _decode_envi(session_dir: Path, full_scale_hint: int | None = None) -> SnapshotResult:
    """Reduce the snapshot segment (ENVI BIL cube: lines x bands x samples) to
    per-band statistics over ALL frames and spatial samples, as % of the
    sensor's full scale (reference: app/reference/reference.py).

    full_scale_hint: ADC full scale derived from the configured pixel_format
    (1023 for Mono10*, 4095 for Mono12*); used only when it is consistent with
    the on-disk storage type, otherwise the data-type default applies."""
    hdrs = sorted(session_dir.glob("segment_*.hdr"))
    if not hdrs:
        return SnapshotResult(False, reason=f"No finalized segment (.hdr) in {session_dir}")
    hdr = hdrs[0]
    bil = hdr.with_suffix(".bil")
    if not bil.is_file():
        return SnapshotResult(False, reason=f"Missing BIL data file for {hdr.name}")

    text = hdr.read_text(errors="replace")
    samples = _hdr_int(text, "samples")
    bands = _hdr_int(text, "bands")
    lines = _hdr_int(text, "lines")
    data_type = _hdr_int(text, "data type")
    if None in (samples, bands, lines, data_type) or 0 in (samples, bands, lines):
        return SnapshotResult(False, reason=f"Bad ENVI header {hdr.name}: samples/bands/lines/data type missing")
    if data_type == 1:
        dtype, full_scale = np.uint8, 255
    elif data_type == 12:
        dtype = np.dtype("<u2")
        # uint16 storage holds either 10- or 12-bit data; only the configured
        # pixel_format can tell them apart. Fall back to 12-bit.
        full_scale = full_scale_hint if full_scale_hint in (1023, 4095) else 4095
    else:
        return SnapshotResult(False, reason=f"Unsupported ENVI data type {data_type}")

    cube = np.fromfile(bil, dtype=dtype)
    if cube.size != lines * bands * samples:
        return SnapshotResult(False, reason=f"BIL size mismatch: {cube.size} px != {lines}x{bands}x{samples}")
    cube = cube.reshape(lines, bands, samples)  # BIL: one frame = one line record

    # Per-band reductions over (frames, samples), normalized to 100%.
    band_axis = (0, 2)
    scale = 100.0 / full_scale
    spectrum_pct = {
        "mean": (cube.mean(axis=band_axis) * scale).tolist(),
        "median": (np.median(cube, axis=band_axis) * scale).tolist(),
        "max": (cube.max(axis=band_axis) * scale).tolist(),
        "min": (cube.min(axis=band_axis) * scale).tolist(),
        "p90": (np.percentile(cube, 90, axis=band_axis) * scale).tolist(),
    }
    return SnapshotResult(
        ok=False,  # caller flips to True after filling metadata
        wavelengths_nm=_hdr_wavelengths(text, bands),
        spectrum_pct={k: [round(float(v), 3) for v in vals] for k, vals in spectrum_pct.items()},
        clipped_pct=float((cube >= full_scale).mean() * 100.0),
        mean_pct=float(cube.mean() * scale),
        lines=lines,
        bands=bands,
        samples=samples,
    )


def _cleanup_old() -> None:
    if not FX10_SNAPSHOT_DIR.is_dir():
        return
    dirs = sorted(p for p in FX10_SNAPSHOT_DIR.iterdir() if p.is_dir())
    for stale in dirs[:-SNAPSHOT_KEEP]:
        shutil.rmtree(stale, ignore_errors=True)
