"""GOX snapshot process orchestration; file/image processing lives in decoder.gox."""

from __future__ import annotations

import asyncio
import cv2
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..decoder.gox import preview
from . import camera_operations, config_store, preview_images, runtime, tool_jobs
from ..constants import (
    BIN_SNAPSHOT,
    SNAPSHOT_CONFIG,
    SNAPSHOT_DIR,
    SNAPSHOT_KEEP,
)

SNAPSHOT_TOOL = "jai_snapshot"
# GUI-side hard timeout; must stay strictly greater than the in-tool
# acquisition.max_duration_s (15 s) so the tool gets to fail first.
SNAPSHOT_TIMEOUT_S = 30.0


@dataclass(frozen=True)
class SnapshotRequest:
    target_ip: str
    exposure_ms: float
    gain: float


@dataclass
class SnapshotResult:
    ok: bool
    reason: str = ""  # FAIL reason / pipeline error
    # preview_images URL paths: 8-bit screen preview, and the on-demand
    # full-resolution 16-bit PNG re-decoded from the snapshot directory.
    image_url: str = ""
    full_image_url: str = ""
    histogram: list[int] = field(default_factory=list)  # 64 bins over native DN [0, 4096)
    clipped_pct: float = 0.0  # native samples at full scale, before demosaic/brightness conversion
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


def guard_reason() -> str | None:
    """Why a camera-touching tool (snapshot, Set IP) must NOT run right now
    (GigE control is exclusive). Discovery itself is fine: it is a broadcast
    the camera answers without a control channel.

    Uses the Enable-GOX value captured at process start — the live file value
    can be toggled mid-run and must not unlock the camera the driver owns.
    """
    return camera_operations.guard_reason("gox")


async def snapshot(ip: str, exposure_ms: float, gain: float) -> SnapshotResult:
    request = SnapshotRequest(ip, exposure_ms, gain)

    async def capture() -> SnapshotResult:
        result = await _snapshot(request.target_ip, request.exposure_ms, request.gain)
        result.request = request
        return result

    return await camera_operations.run("gox", capture, kind="gox-snapshot")


async def _snapshot(ip: str, exposure_ms: float, gain: float) -> SnapshotResult:
    """One full preview shot while the service owns the camera reservation."""
    t0 = time.monotonic()
    sid = time.strftime("%Y%m%d_%H%M%S")
    out_host = SNAPSHOT_DIR / sid
    if out_host.exists():  # same-second collision on rapid consecutive shots
        sid = f"{sid}_{int((time.time() % 1) * 1000):03d}"
        out_host = SNAPSHOT_DIR / sid
    captured = await capture_raw(SnapshotRequest(ip, exposure_ms, gain), out_host)
    if not captured.ok:
        return SnapshotResult(False, reason=captured.reason, raw_output=captured.raw_output,
                              elapsed_s=captured.elapsed_s)
    # Decode only for the GUI, using the same pixel rules as live view.
    result = await asyncio.to_thread(_decode_snapshot, captured.camera_dir)
    result.raw_output = captured.raw_output + "\n--- preview ---\n" + result.raw_output
    result.elapsed_s = time.monotonic() - t0
    if result.ok:
        await asyncio.to_thread(_cleanup_old)
    return result


async def capture_raw(request: SnapshotRequest | None, output: Path, *, config: Path = SNAPSHOT_CONFIG,
                      exposure_ms: float | None = None, gain: float | None = None) -> RawCapture:
    """Capture one RAW frame inside the caller's camera_operations reservation.

    The exposure workbench and colour-reference service share this process path;
    only the workbench applies its temporary-snapshot retention policy.
    A reference passes None to use the saved snapshot config's camera; an
    exposure series adds ``exposure_ms``/``gain`` overrides to that camera.
    """
    if request is not None and (exposure_ms is not None or gain is not None):
        raise ValueError("Exposure overrides come from the request when one is given")
    t0 = time.monotonic()
    # References use a saved copy under the output directory; the caller checked
    # the source config boundary before saving it. Workbench reads the source here.
    if request is not None:
        config = config_store.checked_snapshot_path(config)
    argv = [
        runtime.exec_path(BIN_SNAPSHOT),
        "--config", runtime.exec_path(config),
        "--out", runtime.exec_path(output),
    ]
    if request is not None:
        argv += ["--ip", request.target_ip, "--exposure-ms", str(request.exposure_ms),
                 "--gain", str(request.gain)]
    if exposure_ms is not None:
        argv += ["--exposure-ms", str(float(exposure_ms))]
    if gain is not None:
        argv += ["--gain", str(float(gain))]
    proc = await tool_jobs.spawn(argv)
    try:
        out_b, err_b = await tool_jobs.communicate(proc, SNAPSHOT_TIMEOUT_S)
    except asyncio.TimeoutError:
        return RawCapture(False, reason=f"Timed out (>{SNAPSHOT_TIMEOUT_S:.0f}s) — jai_snapshot was terminated",
                          elapsed_s=time.monotonic() - t0)

    stdout = out_b.decode(errors="replace")
    stderr = err_b.decode(errors="replace")
    raw_output = (stdout + "\n--- stderr ---\n" + stderr).strip()
    if proc.returncode != 0:
        return RawCapture(False, reason=f"Snapshot tool exited with code {proc.returncode}",
                          raw_output=raw_output, elapsed_s=time.monotonic() - t0)

    marker = next((ln for ln in reversed(stdout.splitlines()) if ln.startswith("SNAPSHOT: ")), None)
    if marker is None:
        return RawCapture(False, reason=f"No SNAPSHOT marker in the output (exit {proc.returncode})",
                          raw_output=raw_output, elapsed_s=time.monotonic() - t0)
    if marker.startswith("SNAPSHOT: FAIL"):
        reason = marker[len("SNAPSHOT: FAIL "):]
        if reason.strip().startswith("3"):
            reason += " (connect/control failure — see the raw output for the failing node)"
        return RawCapture(False, reason=reason, raw_output=raw_output,
                          elapsed_s=time.monotonic() - t0)

    if not marker.startswith("SNAPSHOT: OK ") or not marker[len("SNAPSHOT: OK "):].strip():
        return RawCapture(False, reason="Malformed SNAPSHOT result marker", raw_output=raw_output,
                          elapsed_s=time.monotonic() - t0)
    camera_dir_emitted = marker[len("SNAPSHOT: OK "):].strip()
    try:
        camera_dir = runtime.to_host_path(camera_dir_emitted).resolve()
        if camera_dir.parent != output.resolve():
            raise ValueError("Snapshot result is outside the requested camera output directory")
    except ValueError as e:
        return RawCapture(False, reason=str(e), raw_output=raw_output,
                          elapsed_s=time.monotonic() - t0)
    return RawCapture(True, camera_dir=camera_dir, raw_output=raw_output, elapsed_s=time.monotonic() - t0)


def _decode_snapshot(camera_dir) -> SnapshotResult:
    try:
        decoded = preview.snapshot(camera_dir)
        frame, name = decoded.frame.record, decoded.frame.pixel_format
        stem = f"gox_snapshot_{Path(camera_dir).parent.name}_seq{frame.seq}"
        image_url = preview_images.put(decoded.display_png, f"{stem}_screen.png")
        full_image_url = preview_images.lazy(lambda: preview.snapshot_full_png16(camera_dir),
                                             f"{stem}_16bit.png")
        return SnapshotResult(
            ok=True, image_url=image_url, full_image_url=full_image_url, histogram=decoded.histogram,
            clipped_pct=decoded.clipped_pct, mean_dn=decoded.mean_dn,
            decode_name=name, incomplete=decoded.frame.skipped_invalid,
            raw_output=(f"seq={frame.seq} bid={frame.bid} {name} {frame.w}x{frame.h} "
                        f"dts={frame.dts}; decoded for in-memory GUI preview"),
        )
    except (OSError, ValueError, KeyError, TypeError, OverflowError, cv2.error) as error:
        return SnapshotResult(False, reason=f"Decode failed: {error}")


def _cleanup_old() -> None:
    if not SNAPSHOT_DIR.is_dir():
        return
    dirs = sorted(p for p in SNAPSHOT_DIR.iterdir() if p.is_dir())
    for stale in dirs[:-SNAPSHOT_KEEP]:
        shutil.rmtree(stale, ignore_errors=True)
