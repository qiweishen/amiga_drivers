"""GOX snapshot: run jai_snapshot, then decode the frame for the page (decoder.gox)."""

from __future__ import annotations

import asyncio
import cv2
from dataclasses import dataclass, field
from pathlib import Path

from ..decoder.gox import preview
from . import camera_operations, config_store, preview_images, snapshot_tool
from ..constants import BIN_SNAPSHOT, SNAPSHOT_CONFIG, SNAPSHOT_DIR

# Must stay above the tool's own max_duration_s (15 s) so the tool fails first.
SNAPSHOT_TIMEOUT_S = 30.0


@dataclass(frozen=True)
class SnapshotRequest:
    target_ip: str
    exposure_ms: float
    gain: float
    frame_rate_hz: float | None = None  # None keeps the snapshot config's frame_rate_hz


@dataclass
class SnapshotResult:
    ok: bool
    reason: str = ""
    # preview_images URL paths: 8-bit screen preview, and the on-demand
    # full-resolution 16-bit PNG re-decoded from the snapshot directory.
    image_url: str = ""
    full_image_url: str = ""
    histogram: list[int] = field(default_factory=list)  # 64 bins over native DN [0, 4096)
    clipped_pct: float = 0.0  # native samples at full scale, before demosaicing
    mean_dn: float = 0.0
    decode_name: str = ""  # BayerRG12Packed, as recorded by the SDK
    incomplete: bool = False
    elapsed_s: float = 0.0
    raw_output: str = ""
    request: SnapshotRequest | None = None  # requested values, not device readbacks


@dataclass(frozen=True)
class RawCapture:
    ok: bool
    reason: str = ""
    camera_dir: Path | None = None
    raw_output: str = ""
    elapsed_s: float = 0.0


async def snapshot(ip: str, exposure_ms: float, gain: float, frame_rate_hz: float | None = None) -> SnapshotResult:
    request = SnapshotRequest(ip, exposure_ms, gain, frame_rate_hz)

    async def capture() -> SnapshotResult:
        result = await _snapshot(request)
        result.request = request
        return result

    return await camera_operations.run("gox", capture, kind="gox-snapshot")


async def _snapshot(request: SnapshotRequest) -> SnapshotResult:
    """One preview shot while the service owns the camera reservation."""
    captured = await capture_raw(request, snapshot_tool.new_output_dir(SNAPSHOT_DIR))
    if not captured.ok:
        return SnapshotResult(False, reason=captured.reason, raw_output=captured.raw_output,
                              elapsed_s=captured.elapsed_s)
    result = await asyncio.to_thread(_decode_snapshot, captured.camera_dir)
    result.raw_output = captured.raw_output + "\n--- preview ---\n" + result.raw_output
    result.elapsed_s = captured.elapsed_s
    if result.ok:
        await asyncio.to_thread(snapshot_tool.prune, SNAPSHOT_DIR)
    return result


def _argv(request: SnapshotRequest | None, output: Path, config: Path, exposure_ms: float | None,
          gain: float | None, frame_rate_hz: float | None) -> list[str]:
    argv = [str(BIN_SNAPSHOT), "--config", str(config), "--out", str(output)]
    if request is not None:
        argv += ["--ip", request.target_ip]
        exposure_ms, gain, frame_rate_hz = request.exposure_ms, request.gain, request.frame_rate_hz
    for option, value in (("--exposure-ms", exposure_ms), ("--gain", gain), ("--frame-rate-hz", frame_rate_hz)):
        if value is not None:
            argv += [option, str(float(value))]
    return argv


async def capture_raw(request: SnapshotRequest | None, output: Path, *, config: Path = SNAPSHOT_CONFIG,
                      exposure_ms: float | None = None, gain: float | None = None,
                      frame_rate_hz: float | None = None) -> RawCapture:
    """Capture one RAW frame inside the caller's camera reservation.

    The workbench passes a ``request`` (camera and values; the source snapshot
    config). A colour reference passes None and a saved copy of the config; an
    exposure series adds ``exposure_ms`` / ``gain`` overrides to that copy.
    """
    if request is not None and (exposure_ms is not None or gain is not None or frame_rate_hz is not None):
        raise ValueError("Exposure overrides come from the request when one is given")
    if request is not None:
        config = config_store.checked_snapshot_path(config)
    run = await snapshot_tool.run(_argv(request, output, config, exposure_ms, gain, frame_rate_hz),
                                  SNAPSHOT_TIMEOUT_S)
    if not run.ok:
        return RawCapture(False, reason=run.reason, raw_output=run.raw_output, elapsed_s=run.elapsed_s)
    camera_dir = run.output_dir.resolve()
    if camera_dir.parent != output.resolve():
        return RawCapture(False, reason="Snapshot result is outside the requested camera output directory",
                          raw_output=run.raw_output, elapsed_s=run.elapsed_s)
    return RawCapture(True, camera_dir=camera_dir, raw_output=run.raw_output, elapsed_s=run.elapsed_s)


def _decode_snapshot(camera_dir: Path) -> SnapshotResult:
    try:
        decoded = preview.snapshot(camera_dir)
        frame, name = decoded.frame.record, decoded.frame.pixel_format
        stem = f"gox_snapshot_{camera_dir.parent.name}_seq{frame.seq}"
        image_url = preview_images.put(decoded.display_png, f"{stem}_screen.png")
        full_image_url = preview_images.lazy(lambda: preview.snapshot_full_png16(camera_dir), f"{stem}_16bit.png")
        return SnapshotResult(
            ok=True, image_url=image_url, full_image_url=full_image_url, histogram=decoded.histogram,
            clipped_pct=decoded.clipped_pct, mean_dn=decoded.mean_dn,
            decode_name=name, incomplete=decoded.frame.skipped_invalid,
            raw_output=f"seq={frame.seq} bid={frame.bid} {name} {frame.w}x{frame.h} dts={frame.dts}",
        )
    except (OSError, ValueError, KeyError, TypeError, OverflowError, cv2.error) as error:
        return SnapshotResult(False, reason=f"Decode failed: {error}")
