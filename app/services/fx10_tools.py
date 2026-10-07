"""FX10 helpers: the snapshot pipeline (fx10_snapshot in the container ->
shared ENVI BIL reader -> per-band spectral statistics over ~1 s of
frames, normalized to 100%). Device discovery is driver-neutral:
services/ebus_tools.py."""

from __future__ import annotations

import asyncio
import csv
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..constants import (
    BIN_FX10_SNAPSHOT,
    FX10_SNAPSHOT_DIR,
    SNAPSHOT_KEEP,
)
from ..formats import fx10
from . import camera_operations, config_store, runtime, tool_jobs

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


def _decode_envi(session_dir: Path) -> SnapshotResult:
    """Bounded full snapshot; layout/index validation is shared with live/reference."""
    try:
        capture, cube = fx10.snapshot_cube(session_dir)
        values, mean, clipped = fx10.spectrum(cube, capture.full_scale)
        return SnapshotResult(
            ok=False,  # caller fills elapsed/source metadata before declaring success
            wavelengths_nm=list(capture.wavelengths_nm), spectrum_pct=values,
            clipped_pct=clipped, mean_pct=mean, lines=cube.shape[0],
            bands=capture.bands, samples=capture.samples,
        )
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError, csv.Error) as error:
        return SnapshotResult(False, reason=f"Cannot decode recorded capture: {error}")


def _cleanup_old() -> None:
    if not FX10_SNAPSHOT_DIR.is_dir():
        return
    dirs = sorted(p for p in FX10_SNAPSHOT_DIR.iterdir() if p.is_dir())
    for stale in dirs[:-SNAPSHOT_KEEP]:
        shutil.rmtree(stale, ignore_errors=True)
