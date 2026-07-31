"""FX10 helpers: eBUS device discovery (fx10_snapshot --list) and the snapshot
pipeline (fx10_snapshot in the container -> ENVI BIL decode on the host ->
8-bit waterfall JPEG for the browser + brightness histogram)."""

from __future__ import annotations

import asyncio
import base64
import json
import re
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np

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
    jpeg_b64: str = ""  # data-URL payload for ui.interactive_image
    histogram: list[int] = field(default_factory=list)  # 64 bins over the raw range
    clipped_pct: float = 0.0  # saturated pixels over the FULL cube
    mean_pct: float = 0.0  # mean of the displayed image as % of full scale
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


async def snapshot(ip: str, exposure_ms: float, frames: int, band_mode: str) -> SnapshotResult:
    """One full preview waterfall. Caller must have checked guard_reason() and
    must serialize calls (STATE.snapshot_busy)."""
    t0 = time.monotonic()
    timeout_s = _timeout_s(frames, exposure_ms)
    sid = time.strftime("%Y%m%d_%H%M%S")
    out_host = FX10_SNAPSHOT_DIR / sid
    if out_host.exists():  # same-second collision on rapid auto-refresh
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

    # Decode on the host (venv has numpy + opencv): ENVI BIL -> waterfall JPEG.
    result = await asyncio.to_thread(_decode_envi, session_dir, band_mode)
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


def _decode_envi(session_dir: Path, band_mode: str) -> SnapshotResult:
    """Decode the single snapshot segment: ENVI BIL cube (lines, bands, samples)
    collapsed to an 8-bit waterfall (lines x samples) + raw-range stats."""
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
        dtype, full_scale = np.dtype("<u2"), 4095  # Mono10/Mono12 in uint16; 12-bit scale
    else:
        return SnapshotResult(False, reason=f"Unsupported ENVI data type {data_type}")

    cube = np.fromfile(bil, dtype=dtype)
    if cube.size != lines * bands * samples:
        return SnapshotResult(False, reason=f"BIL size mismatch: {cube.size} px != {lines}x{bands}x{samples}")
    cube = cube.reshape(lines, bands, samples)  # BIL: one frame = one line record

    if band_mode == "mean":
        img = cube.mean(axis=1)
    elif band_mode.startswith("band:"):
        try:
            band = int(band_mode[len("band:"):])
        except ValueError:
            return SnapshotResult(False, reason=f"Bad band mode {band_mode!r}")
        if not 0 <= band < bands:
            return SnapshotResult(False, reason=f"Band {band} out of range (0..{bands - 1})")
        img = cube[:, band, :].astype(np.float64)
    else:
        return SnapshotResult(False, reason=f"Bad band mode {band_mode!r}")

    img8 = np.clip(img / full_scale * 255.0, 0, 255).astype(np.uint8)
    ok, jpg = cv2.imencode(".jpg", img8, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
    if not ok:
        return SnapshotResult(False, reason="JPEG encoding failed")
    hist, _ = np.histogram(img, bins=64, range=(0, full_scale))
    return SnapshotResult(
        ok=False,  # caller flips to True after filling metadata
        jpeg_b64=base64.b64encode(jpg.tobytes()).decode(),
        histogram=[int(v) for v in hist],
        clipped_pct=float((cube >= full_scale).mean() * 100.0),
        mean_pct=float(img.mean() / full_scale * 100.0),
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
