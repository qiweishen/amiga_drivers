"""GoX helpers: the snapshot pipeline (jai_snapshot in the container ->
shared jai_raw decoder -> 8-bit JPEG for the browser + brightness
histogram). Device discovery is driver-neutral: services/ebus_tools.py."""

from __future__ import annotations

import asyncio
import base64
import shutil
import time
from dataclasses import dataclass, field

import cv2
import numpy as np

from ..constants import (
    BIN_SNAPSHOT,
    SNAPSHOT_CONFIG,
    SNAPSHOT_DIR,
    SNAPSHOT_KEEP,
)
from gox_driver.scripts.jai_raw import pixels, recording

from . import camera_operations, runtime, tool_jobs

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
    jpeg_b64: str = ""  # data-URL payload for ui.image
    histogram: list[int] = field(default_factory=list)  # 64 bins over 16-bit range
    clipped_pct: float = 0.0  # native samples at full scale, before demosaic/brightness conversion
    mean_16: float = 0.0
    decode_name: str = ""  # e.g. BayerRG12Packed (from the shared PFNC registry)
    incomplete: bool = False
    elapsed_s: float = 0.0
    raw_output: str = ""
    request: SnapshotRequest | None = None  # requested values, not device readbacks


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
    proc = await tool_jobs.spawn([
        runtime.exec_path(BIN_SNAPSHOT),
        "--config", runtime.exec_path(SNAPSHOT_CONFIG),
        "--out", runtime.exec_path(out_host),
        "--ip", ip,
        "--exposure-ms", str(exposure_ms),
        "--gain", str(gain),
    ])
    try:
        out_b, err_b = await tool_jobs.communicate(proc, SNAPSHOT_TIMEOUT_S)
    except asyncio.TimeoutError:
        return SnapshotResult(False, reason=f"Timed out (>{SNAPSHOT_TIMEOUT_S:.0f}s) — jai_snapshot was terminated",
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

    camera_dir_emitted = marker[len("SNAPSHOT: OK "):].strip()
    try:
        camera_dir = runtime.to_host_path(camera_dir_emitted)
    except ValueError as e:
        return SnapshotResult(False, reason=str(e), raw_output=raw_output,
                              elapsed_s=time.monotonic() - t0)

    # Decode in-process through the same pure library as the CLI and live view.
    result = await asyncio.to_thread(_decode_snapshot, camera_dir, out_host)
    result.raw_output = raw_output + "\n--- unpack ---\n" + result.raw_output
    result.elapsed_s = time.monotonic() - t0
    if result.ok:
        await asyncio.to_thread(_cleanup_old)
    return result


def _decode_snapshot(camera_dir, output_dir) -> SnapshotResult:
    try:
        frame, payload, incomplete = recording.first_preview_frame(camera_dir)
        image, name, _ = pixels.decode(frame.pf, frame.w, frame.h, payload)
        # Retain the existing native PNG artifact and filename; statistics use
        # the native array directly instead of decoding that PNG a second time.
        png = output_dir / f"seq{frame.seq:08d}_{name}.png"
        if not cv2.imwrite(str(png), image):
            raise ValueError("Cannot write the decoded PNG")
        result = _encode_and_histogram(image, name)
        result.decode_name = name
        result.incomplete = incomplete
        result.raw_output = (f"seq={frame.seq} bid={frame.bid} {name} {frame.w}x{frame.h} "
                             f"dts={frame.dts} -> {png}\nunpacked 1 frame(s) into {output_dir.resolve()}")
        return result
    except (OSError, ValueError, KeyError, TypeError, OverflowError, cv2.error) as error:
        return SnapshotResult(False, reason=f"Decode failed: {error}")


# Keep the service-level helper name for callers; its rules live only in pixels.
_prepare_snapshot_image = pixels.prepare_snapshot_image


def _encode_and_histogram(img: np.ndarray, decode_name: str) -> SnapshotResult:
    try:
        img, clipped_pct = _prepare_snapshot_image(img, decode_name)
    except (ValueError, cv2.error) as e:
        return SnapshotResult(False, reason=f"Cannot prepare snapshot: {e}")
    if img.dtype == np.uint16:
        img8 = (img >> 8).astype(np.uint8)
        gray16 = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
    else:
        img8 = img
        gray8 = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
        gray16 = gray8.astype(np.uint16) << 8
    ok, jpg = cv2.imencode(".jpg", img8, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
    if not ok:
        return SnapshotResult(False, reason="JPEG encoding failed")
    hist, _ = np.histogram(gray16, bins=64, range=(0, 65536))
    return SnapshotResult(
        ok=True,
        jpeg_b64=base64.b64encode(jpg.tobytes()).decode(),
        histogram=[int(v) for v in hist],
        clipped_pct=clipped_pct,
        mean_16=float(gray16.mean()),
    )


def _cleanup_old() -> None:
    if not SNAPSHOT_DIR.is_dir():
        return
    dirs = sorted(p for p in SNAPSHOT_DIR.iterdir() if p.is_dir())
    for stale in dirs[:-SNAPSHOT_KEEP]:
        shutil.rmtree(stale, ignore_errors=True)
