"""FX10 helpers: the snapshot pipeline (fx10_snapshot in the container ->
ENVI BIL decode on the host -> per-band spectral statistics over ~1 s of
frames, normalized to 100%). Device discovery is driver-neutral:
services/ebus_tools.py."""

from __future__ import annotations

import asyncio
import csv
import json
import re
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from ..constants import (
    BIN_FX10_SNAPSHOT,
    FX10_SNAPSHOT_DIR,
    SNAPSHOT_KEEP,
)
from . import camera_operations, config_store, runtime, tool_jobs, control_client, wire

SNAPSHOT_TOOL = "fx10_snapshot"
# Freerun rate forced inside the tool: min(its --fps default, 1000/exposure_ms)
# — long exposures lower the achievable rate. The GUI-side hard timeout must
# stay strictly greater than the in-tool wall-clock budget
# (frames / effective_fps * 3 + 5 s) so the tool gets to fail first.
SNAPSHOT_FPS = 50.0


def _timeout_s(frames: int, exposure_ms: float) -> float:
    effective_fps = min(SNAPSHOT_FPS, 1000.0 / max(exposure_ms, 0.01))
    return frames / effective_fps * 3 + 10


@dataclass(frozen=True)
class SnapshotRequest:
    target_ip: str
    exposure_ms: float
    spatial_binning: int | None
    spectral_binning: int | None


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
    request: SnapshotRequest | None = None  # requested values, not device readbacks


def guard_reason() -> str | None:
    """Why a camera-touching tool (snapshot, Set IP) must NOT run right now
    (GigE control is exclusive). Discovery itself is fine: it is a broadcast
    the camera answers without a control channel.

    Uses the Enable-FX10 value captured at process start — the live file value
    can be toggled mid-run and must not unlock the camera the driver owns.
    """
    return camera_operations.guard_reason("fx10")


async def snapshot(ip: str, exposure_ms: float,
                   spatial_binning: int | None = None,
                   spectral_binning: int | None = None) -> SnapshotResult:
    if control_client.enabled():
        return wire.restore(SnapshotResult, await control_client.call("fx10.snapshot", ip, exposure_ms,
                                                                     spatial_binning, spectral_binning))
    request = SnapshotRequest(ip, exposure_ms, spatial_binning, spectral_binning)

    async def capture() -> SnapshotResult:
        result = await _snapshot(request.target_ip, request.exposure_ms,
                                 request.spatial_binning, request.spectral_binning)
        result.request = request
        return result

    return await camera_operations.run("fx10", capture, kind="fx10-snapshot")


async def _snapshot(ip: str, exposure_ms: float,
                    spatial_binning: int | None,
                    spectral_binning: int | None) -> SnapshotResult:
    """One spectral preview: capture ~1 second of frames and reduce them to
    per-band statistics while the service owns the camera reservation.

    The binning overrides let the page preview a setting WITHOUT writing it to
    config-fx10.yaml (which is also the live recording config) — persisting is
    the explicit job of the Apply button. None = keep the config's value."""
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
    argv = [
        runtime.exec_path(BIN_FX10_SNAPSHOT),
        "--config", runtime.exec_path(config_store.get("fx10").path),
        "--out", runtime.exec_path(out_host),
        "--ip", ip,
        "--frames", str(frames),
        "--exposure-ms", str(exposure_ms),
    ]
    if spatial_binning is not None:
        argv += ["--spatial-binning", str(spatial_binning)]
    if spectral_binning is not None:
        argv += ["--spectral-binning", str(spectral_binning)]
    proc = await tool_jobs.spawn(argv)
    try:
        out_b, err_b = await tool_jobs.communicate(proc, timeout_s)
    except asyncio.TimeoutError:
        return SnapshotResult(False, reason=f"Timed out (>{timeout_s:.0f}s) — fx10_snapshot was terminated",
                              elapsed_s=time.monotonic() - t0)

    stdout = out_b.decode(errors="replace")
    stderr = err_b.decode(errors="replace")
    raw_output = (stdout + "\n--- stderr ---\n" + stderr).strip()
    if proc.returncode != 0:
        return SnapshotResult(False, reason=f"Snapshot tool exited with code {proc.returncode}",
                              raw_output=raw_output, elapsed_s=time.monotonic() - t0)

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

    # Decode using the metadata saved with this capture, even if settings changed later.
    result = await asyncio.to_thread(_decode_envi, session_dir)
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


def _hdr_wavelengths(text: str, bands: int) -> list[float]:
    """Only return an explicit finite header axis. Missing calibration stays missing."""
    m = re.search(r"^wavelength\s*=\s*\{([^}]*)\}", text, re.MULTILINE | re.IGNORECASE | re.DOTALL)
    if m:
        try:
            values = [float(v) for v in m.group(1).replace("\n", " ").split(",") if v.strip()]
            if len(values) == bands and all(np.isfinite(v) and v > 0 for v in values):
                return values
        except ValueError:
            pass
    return []


def _decode_envi(session_dir: Path) -> SnapshotResult:
    """Reduce the snapshot segment (ENVI BIL cube: lines x bands x samples) to
    statistics over genuine, non-anomalous frames and spatial samples.
    Bit depth comes only from immutable capture metadata. This bounded preview
    refuses oversized inputs instead of partially interpreting a snapshot."""
    hdrs = sorted(session_dir.glob("segment_*.hdr"))
    if not hdrs:
        return SnapshotResult(False, reason=f"No finalized segment (.hdr) in {session_dir}")
    if len(hdrs) != 1:
        return SnapshotResult(False, reason="Snapshot unexpectedly contains multiple segments")
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
    try:
        capture = json.loads((session_dir / "capture.json").read_text(encoding="utf-8"))
        full_scale = int(capture["full_scale"])
        bpp = int(capture["bytes_per_pixel"])
        if (capture.get("format") != "fx10-capture-v1" or capture.get("byte_order") != "little"
                or _hdr_int(text, "byte order") != 0 or _hdr_int(text, "header offset") != 0
                or int(capture["samples"]) != samples or int(capture["bands"]) != bands
                or (data_type, bpp, full_scale) not in ((1, 1, 255), (12, 2, 1023), (12, 2, 4095))):
            raise ValueError("Capture metadata and ENVI layout disagree")
        line_bytes = samples * bands * bpp
        if lines > 4096 or lines * line_bytes > 256 * 1024 * 1024:
            raise ValueError("Snapshot exceeds preview memory budget")
        if bil.stat().st_size != lines * line_bytes:
            raise ValueError("BIL byte length disagrees with the finalized header")
        valid_lines = []
        with hdr.with_suffix(".lines.csv").open(encoding="ascii", newline="") as index:
            if not index.readline().startswith(("# fx10-line-index-v1;", "# fx10-line-index-v2;")):
                raise ValueError("Missing versioned line identity index")
            for row in csv.DictReader(index):
                if row["event"] != "frame" or row["block_id_anomaly"] != "0":
                    continue
                n = int(row["segment_line"])
                if not 0 <= n < lines or int(row["byte_offset"]) != n * line_bytes:
                    raise ValueError("Invalid line index offset")
                if valid_lines and n <= valid_lines[-1]:
                    raise ValueError("Duplicate or reversed line index offset")
                valid_lines.append(n)
        if not valid_lines:
            raise ValueError("No non-anomalous camera frames in the line index")
        dtype = np.dtype("u1") if bpp == 1 else np.dtype("<u2")
        cube = np.fromfile(bil, dtype=dtype, count=lines * bands * samples)
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as e:
        return SnapshotResult(False, reason=f"Cannot decode recorded capture: {e}")
    if cube.size != lines * bands * samples:
        return SnapshotResult(False, reason=f"BIL size mismatch: {cube.size} px != {lines}x{bands}x{samples}")
    cube = cube.reshape(lines, bands, samples)  # BIL: one frame = one line record
    cube = cube[valid_lines]

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
        lines=len(valid_lines),
        bands=bands,
        samples=samples,
    )


def _cleanup_old() -> None:
    if not FX10_SNAPSHOT_DIR.is_dir():
        return
    dirs = sorted(p for p in FX10_SNAPSHOT_DIR.iterdir() if p.is_dir())
    for stale in dirs[:-SNAPSHOT_KEEP]:
        shutil.rmtree(stale, ignore_errors=True)
