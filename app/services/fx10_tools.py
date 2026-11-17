"""FX10 snapshot: run fx10_snapshot for ~1 s of frames, then reduce them to
per-band statistics in raw DN (decoder.fx10). Discovery is in services/ebus_tools.py."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from pathlib import Path

import yaml

from . import camera_operations, config_store, snapshot_tool, timing
from ..constants import BIN_FX10_SNAPSHOT, FX10_SNAPSHOT_CONFIG, FX10_SNAPSHOT_DIR
from ..decoder import fx10

# Default only when the YAML omits the rate; matches fx10::AcquisitionConfig.
DEFAULT_FRAME_RATE_HZ = 50.0


def _capture_plan(exposure_ms: float, config: Path | None = None, *, frame_rate_hz: float | None = None,
                  spatial_binning: int | None = None,
                  spectral_binning: int | None = None) -> tuple[Path, float, int, float]:
    """(config, rate, frame count, GUI timeout) for one ~1 s capture.

    ``config`` is a preserved copy of the snapshot configuration (exposure series).
    ``frame_rate_hz`` is the workbench rate; None takes the config rate, lowered to the
    highest rate at which ``exposure_ms`` still fits (exposure series). Binning None keeps
    the config's value. fx10_snapshot applies the same checks and never caps silently.
    """
    config = config_store.checked_snapshot_path(FX10_SNAPSHOT_CONFIG) if config is None else config
    document = yaml.safe_load(config.read_bytes()) or {}
    acquisition = document.get("acquisition", {}) if isinstance(document, dict) else None
    if not isinstance(acquisition, dict):
        raise ValueError("FX10 snapshot configuration and its acquisition must be YAML mappings")
    spatial = acquisition.get("spatial_binning", 1) if spatial_binning is None else spatial_binning
    spectral = acquisition.get("spectral_binning", 2) if spectral_binning is None else spectral_binning
    rate = acquisition.get("frame_rate_hz", DEFAULT_FRAME_RATE_HZ) if frame_rate_hz is None else frame_rate_hz
    if any(isinstance(value, bool) for value in (rate, exposure_ms, spatial, spectral)):
        raise ValueError("FX10 frame rate, exposure and binning must be numbers")
    rate, exposure_ms = timing.positive(rate, "frame rate"), timing.fx10_check_exposure(exposure_ms)
    spatial, spectral = int(spatial), int(spectral)
    if frame_rate_hz is None:
        rate = min(rate, timing.fx10_max_frame_rate_hz(exposure_ms, spatial, spectral))
    timing.fx10_check_frame_rate(rate, spatial, spectral)
    if not timing.exposure_fits(exposure_ms, rate):
        raise ValueError(timing.period_error(exposure_ms, rate))
    frames = max(1, round(rate))
    # The tool's own deadline is frames / rate * 3 + 5 s; give it 5 s more to finish first.
    return config, rate, frames, frames / rate * 3 + 10


@dataclass(frozen=True)
class SnapshotRequest:
    target_ip: str
    exposure_ms: float
    spatial_binning: int | None
    spectral_binning: int | None
    frame_rate_hz: float | None = None  # None = the snapshot config's rate


@dataclass
class SnapshotResult:
    ok: bool
    reason: str = ""
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


async def snapshot(ip: str, exposure_ms: float, spatial_binning: int | None = None,
                   spectral_binning: int | None = None, frame_rate_hz: float | None = None) -> SnapshotResult:
    request = SnapshotRequest(ip, exposure_ms, spatial_binning, spectral_binning, frame_rate_hz)

    async def capture() -> SnapshotResult:
        result = await _snapshot(ip, exposure_ms, spatial_binning, spectral_binning, frame_rate_hz)
        result.request = request
        return result

    return await camera_operations.run("fx10", capture, kind="fx10-snapshot")


async def _snapshot(ip: str, exposure_ms: float, spatial_binning: int | None, spectral_binning: int | None,
                    frame_rate_hz: float | None = None) -> SnapshotResult:
    """~1 s of frames reduced to per-band statistics, inside the camera reservation.

    Binning and rate overrides preview a setting without writing any config
    (Apply does that); None keeps the config's value."""
    config, frame_rate_hz, frames, timeout_s = await asyncio.to_thread(
        lambda: _capture_plan(exposure_ms, frame_rate_hz=frame_rate_hz,
                              spatial_binning=spatial_binning, spectral_binning=spectral_binning))
    argv = [str(BIN_FX10_SNAPSHOT), "--config", str(config), "--out", str(snapshot_tool.new_output_dir(FX10_SNAPSHOT_DIR)),
            "--ip", ip, "--frames", str(frames),
            # Freeze the planned rate even if the YAML changes before the tool opens it.
            "--fps", str(frame_rate_hz), "--exposure-ms", str(exposure_ms)]
    if spatial_binning is not None:
        argv += ["--spatial-binning", str(spatial_binning)]
    if spectral_binning is not None:
        argv += ["--spectral-binning", str(spectral_binning)]
    run = await snapshot_tool.run(argv, timeout_s)
    if not run.ok:
        return SnapshotResult(False, reason=run.reason, raw_output=run.raw_output, elapsed_s=run.elapsed_s)
    # Decode with the metadata saved with this capture, even if settings changed later.
    result = await asyncio.to_thread(_decode_raw, run.output_dir)
    result.raw_output, result.elapsed_s = run.raw_output, run.elapsed_s
    if result.ok:
        await asyncio.to_thread(snapshot_tool.prune, FX10_SNAPSHOT_DIR)
    return result


async def capture_raw(output: Path, exposure_ms: float, *, config: Path) -> SnapshotResult:
    """Capture ~1 s at ``exposure_ms`` into ``output`` without decoding it.

    For the exposure series, which already holds the camera reservation. The
    camera and binning come from ``config`` (a preserved copy of the snapshot
    configuration); only the exposure is overridden.
    """
    config, frame_rate_hz, frames, timeout_s = await asyncio.to_thread(_capture_plan, exposure_ms, config)
    run = await snapshot_tool.run([str(BIN_FX10_SNAPSHOT), "--config", str(config), "--out", str(output),
                                   "--frames", str(frames), "--fps", str(frame_rate_hz),
                                   "--exposure-ms", str(float(exposure_ms))], timeout_s)
    return SnapshotResult(run.ok, reason=run.reason, raw_output=run.raw_output, elapsed_s=run.elapsed_s,
                          session_dir=str(run.output_dir) if run.ok else "")


def _decode_raw(session_dir: Path) -> SnapshotResult:
    try:
        capture, cube = fx10.snapshot_cube(session_dir)
        values, mean, clipped = fx10.spectrum(cube, capture.full_scale)
        return SnapshotResult(
            ok=True, wavelengths_nm=list(capture.wavelengths_nm), spectrum_dn=values,
            full_scale=capture.full_scale, pixel_format=capture.pixel_format,
            clipped_pct=clipped, mean_dn=mean, lines=cube.shape[0],
            bands=capture.bands, samples=capture.samples, session_dir=str(session_dir),
        )
    except (OSError, ValueError, KeyError, TypeError, OverflowError, UnicodeError) as error:
        return SnapshotResult(False, reason=f"Cannot decode recorded capture: {error}")
