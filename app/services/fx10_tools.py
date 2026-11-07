"""FX10 helpers: the snapshot pipeline (fx10_snapshot in the container ->
shared ebus-raw reader -> per-band spectral statistics over ~1 s of
frames in raw digital numbers). Device discovery is driver-neutral:
services/ebus_tools.py."""

from __future__ import annotations

import asyncio
import math
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

import yaml

from . import camera_operations, config_store, runtime, tool_jobs
from ..constants import (
    BIN_FX10_SNAPSHOT,
    FX10_SNAPSHOT_CONFIG,
    FX10_SNAPSHOT_DIR,
    SNAPSHOT_KEEP,
)
from ..decoder import fx10

SNAPSHOT_TOOL = "fx10_snapshot"
MAX_CONFIG_BYTES = 1024 * 1024
# Default only when the YAML omits the rate; matches fx10::AcquisitionConfig.
DEFAULT_FRAME_RATE_HZ = 50.0


def _capture_plan(exposure_ms: float, config: Path | None = None) -> tuple[Path, float, int, float]:
    """Read the snapshot rate and match the tool's exposure cap and time budget.

    ``config`` is a preserved copy of the snapshot configuration (exposure series).
    """
    config = config_store.checked_snapshot_path(FX10_SNAPSHOT_CONFIG) if config is None else config
    with config.open("rb") as source:
        raw = source.read(MAX_CONFIG_BYTES + 1)
    if len(raw) > MAX_CONFIG_BYTES:
        raise ValueError("FX10 snapshot configuration exceeds the capture limit")
    document = yaml.safe_load(raw)
    if document is None:
        document = {}
    if not isinstance(document, dict):
        raise ValueError("FX10 snapshot configuration must be a YAML mapping")
    acquisition = document.get("acquisition", {})
    if not isinstance(acquisition, dict):
        raise ValueError("FX10 snapshot acquisition must be a YAML mapping")
    rate = acquisition.get("frame_rate_hz", DEFAULT_FRAME_RATE_HZ)
    try:
        if isinstance(rate, bool) or isinstance(exposure_ms, bool):
            raise ValueError
        frame_rate_hz, exposure_ms = float(rate), float(exposure_ms)
    except (TypeError, ValueError, OverflowError) as error:
        raise ValueError("FX10 frame rate and exposure must be finite positive numbers") from error
    if not all(math.isfinite(value) and value > 0 for value in (frame_rate_hz, exposure_ms)):
        raise ValueError("FX10 frame rate and exposure must be finite positive numbers")
    effective_fps = min(frame_rate_hz, 1000.0 / exposure_ms)
    frames = max(1, round(effective_fps))
    tool_duration = frames / effective_fps * 3 + 5
    if not math.isfinite(tool_duration) or tool_duration > 604800:
        raise ValueError("FX10 snapshot duration exceeds seven days")
    # Let the tool's recording deadline finish before the GUI terminates it.
    return config, frame_rate_hz, frames, tool_duration + 5


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
    # Per-band statistics over ALL captured frames and samples, in raw DN.
    spectrum_dn: dict[str, list[float]] = field(default_factory=dict)
    full_scale: int = 0  # effective PixelFormat range, not storage-container width
    pixel_format: str = ""
    clipped_pct: float = 0.0  # saturated pixels over the FULL cube
    mean_dn: float = 0.0  # cube mean in raw DN
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
    config, frame_rate_hz, frames, timeout_s = await asyncio.to_thread(_capture_plan, exposure_ms)
    sid = time.strftime("%Y%m%d_%H%M%S")
    out_host = FX10_SNAPSHOT_DIR / sid
    if out_host.exists():  # same-second collision on rapid consecutive shots
        sid = f"{sid}_{int((time.time() % 1) * 1000):03d}"
        out_host = FX10_SNAPSHOT_DIR / sid
    argv = [
        runtime.exec_path(BIN_FX10_SNAPSHOT),
        "--config", runtime.exec_path(config),
        "--out", runtime.exec_path(out_host),
        "--ip", ip,
        "--frames", str(frames),
        # Freeze the planned rate even if the source YAML is edited before the
        # tool opens it. The C++ tool still validates the full configuration.
        "--fps", str(frame_rate_hz),
        "--exposure-ms", str(exposure_ms),
    ]
    if spatial_binning is not None:
        argv += ["--spatial-binning", str(spatial_binning)]
    if spectral_binning is not None:
        argv += ["--spectral-binning", str(spectral_binning)]
    session_dir, raw_output, failure = await _invoke(argv, timeout_s, t0)
    if failure is not None:
        return failure
    # Decode using the metadata saved with this capture, even if settings changed later.
    result = await asyncio.to_thread(_decode_raw, session_dir)
    result.raw_output = raw_output
    result.elapsed_s = time.monotonic() - t0
    if result.reason:
        return result
    result.ok = True
    result.session_dir = str(session_dir)

    await asyncio.to_thread(_cleanup_old)
    return result


async def _invoke(argv: list[str], timeout_s: float, t0: float) -> tuple[Path | None, str, SnapshotResult | None]:
    """Run fx10_snapshot; (session_dir, raw_output, None) on success, else (None, raw_output, failure)."""
    proc = await tool_jobs.spawn(argv)
    try:
        out_b, err_b = await tool_jobs.communicate(proc, timeout_s)
    except asyncio.TimeoutError:
        return None, "", SnapshotResult(False, reason=f"Timed out (>{timeout_s:.0f}s) — fx10_snapshot was terminated",
                                        elapsed_s=time.monotonic() - t0)

    stdout = out_b.decode(errors="replace")
    stderr = err_b.decode(errors="replace")
    raw_output = (stdout + "\n--- stderr ---\n" + stderr).strip()

    def failed(reason: str):
        return None, raw_output, SnapshotResult(False, reason=reason, raw_output=raw_output,
                                                elapsed_s=time.monotonic() - t0)

    if proc.returncode != 0:
        return failed(f"Snapshot tool exited with code {proc.returncode}")
    marker = next((ln for ln in reversed(stdout.splitlines()) if ln.startswith("SNAPSHOT: ")), None)
    if marker is None:
        return failed(f"No SNAPSHOT marker in the output (exit {proc.returncode})")
    if marker.startswith("SNAPSHOT: FAIL"):
        reason = marker[len("SNAPSHOT: FAIL "):]
        if reason.strip().startswith("3"):
            reason += " (connect/control failure — see the raw output for the failing node)"
        return failed(reason)
    try:
        return runtime.to_host_path(marker[len("SNAPSHOT: OK "):].strip()), raw_output, None
    except ValueError as e:
        return failed(str(e))


async def capture_raw(output: Path, exposure_ms: float, *, config: Path) -> SnapshotResult:
    """Capture ~1 s at ``exposure_ms`` into ``output`` without decoding it.

    For the exposure series, whose caller already holds the camera reservation.
    The camera address and binning come from ``config`` (a preserved copy of the
    snapshot configuration); only the exposure is overridden. On success
    ``session_dir`` names the finalized recording.
    """
    t0 = time.monotonic()
    config, frame_rate_hz, frames, timeout_s = await asyncio.to_thread(_capture_plan, exposure_ms, config)
    argv = [
        runtime.exec_path(BIN_FX10_SNAPSHOT),
        "--config", runtime.exec_path(config),
        "--out", runtime.exec_path(output),
        "--frames", str(frames),
        "--fps", str(frame_rate_hz),
        "--exposure-ms", str(float(exposure_ms)),
    ]
    session_dir, raw_output, failure = await _invoke(argv, timeout_s, t0)
    if failure is not None:
        return failure
    return SnapshotResult(True, raw_output=raw_output, session_dir=str(session_dir),
                          elapsed_s=time.monotonic() - t0)


def _decode_raw(session_dir: Path) -> SnapshotResult:
    """Bounded raw snapshot; profile validation is shared with live/reference."""
    try:
        capture, cube = fx10.snapshot_cube(session_dir)
        values, mean, clipped = fx10.spectrum(cube, capture.full_scale)
        return SnapshotResult(
            ok=False,  # caller fills elapsed/source metadata before declaring success
            wavelengths_nm=list(capture.wavelengths_nm), spectrum_dn=values,
            full_scale=capture.full_scale, pixel_format=capture.pixel_format,
            clipped_pct=clipped, mean_dn=mean, lines=cube.shape[0],
            bands=capture.bands, samples=capture.samples,
        )
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError) as error:
        return SnapshotResult(False, reason=f"Cannot decode recorded capture: {error}")


def _cleanup_old() -> None:
    if not FX10_SNAPSHOT_DIR.is_dir():
        return
    dirs = sorted(p for p in FX10_SNAPSHOT_DIR.iterdir() if p.is_dir())
    for stale in dirs[:-SNAPSHOT_KEEP]:
        shutil.rmtree(stale, ignore_errors=True)
