"""GoX helpers: camera discovery (jai_discover --json) and the snapshot
pipeline (jai_snapshot in the container -> unpack_raw.py on the host ->
8-bit JPEG for the browser + brightness histogram)."""

from __future__ import annotations

import asyncio
import base64
import json
import shutil
import subprocess
import time
from dataclasses import dataclass, field

import cv2
import numpy as np

from ..constants import (
    BIN_DISCOVER,
    BIN_SNAPSHOT,
    REPO_ROOT,
    SNAPSHOT_CONFIG_CONTAINER,
    SNAPSHOT_DIR,
    SNAPSHOT_KEEP,
    UNPACK_SCRIPT,
    VENV_PYTHON,
    to_host,
)
from ..state import STATE, ProcState
from . import docker_runner

SNAPSHOT_TOOL = "jai_snapshot"
# GUI-side hard timeout; must stay strictly greater than the in-tool
# acquisition.max_duration_s (15 s) so the tool gets to fail first.
SNAPSHOT_TIMEOUT_S = 30.0
UNPACK_TIMEOUT_S = 30.0
CLIP_THRESHOLD = 0xFFF0  # a saturated 12-bit pixel after --shift-to-16bit


@dataclass
class Device:
    model: str
    ip: str
    mac: str
    serial: str
    user_name: str
    config_valid: bool


@dataclass
class DiscoverResult:
    devices: list[Device] = field(default_factory=list)
    raw_output: str = ""
    error: str = ""
    json_supported: bool = True


@dataclass
class SnapshotResult:
    ok: bool
    reason: str = ""  # FAIL reason / pipeline error
    jpeg_b64: str = ""  # data-URL payload for ui.image
    histogram: list[int] = field(default_factory=list)  # 64 bins over 16-bit range
    clipped_pct: float = 0.0
    mean_16: float = 0.0
    decode_name: str = ""  # e.g. BayerRG12Packed (from the PNG filename)
    incomplete: bool = False
    elapsed_s: float = 0.0
    raw_output: str = ""


def guard_reason() -> str | None:
    """Why snapshot/discover must NOT run right now (GigE control is exclusive).

    Uses the Enable-GOX value captured at process start — the live file value
    can be toggled mid-run and must not unlock the camera the driver owns.
    """
    if STATE.process_state in (ProcState.RUNNING, ProcState.STARTING, ProcState.STOPPING):
        if STATE.enables_at_start.get("gox", False):
            return "采集进程正在运行且启用了 GoX —— 相机被其独占，请先停止采集"
    return None


async def discover(timeout_ms: int = 1500) -> DiscoverResult:
    res = await docker_runner.exec_(
        [BIN_DISCOVER, "--timeout", str(timeout_ms), "--json"], timeout=timeout_ms / 1000 + 15
    )
    raw = (res.stdout + ("\n" + res.stderr if res.stderr.strip() else "")).strip()
    if res.code == 2 and "unknown argument" in res.stderr:
        # Old binary without --json: degrade to the human-readable table.
        res = await docker_runner.exec_(
            [BIN_DISCOVER, "--timeout", str(timeout_ms)], timeout=timeout_ms / 1000 + 15
        )
        return DiscoverResult(raw_output=res.stdout + res.stderr, json_supported=False,
                              error="" if res.ok else "jai_discover 失败（旧版二进制，无 --json）")
    if not res.ok:
        return DiscoverResult(raw_output=raw, error=f"jai_discover 失败 (exit {res.code}): {res.stderr.strip()}")
    try:
        doc = json.loads(res.stdout)
    except json.JSONDecodeError as e:
        return DiscoverResult(raw_output=raw, error=f"无法解析 --json 输出: {e}")
    devices = [
        Device(
            model=d.get("model", ""),
            ip=d.get("ip", ""),
            mac=d.get("mac", ""),
            serial=d.get("serial", ""),
            user_name=d.get("user_name", ""),
            config_valid=bool(d.get("config_valid", False)),
        )
        for iface in doc.get("interfaces", [])
        for d in iface.get("devices", [])
    ]
    return DiscoverResult(devices=devices, raw_output=raw)


async def snapshot(ip: str, exposure_us: float, gain: float) -> SnapshotResult:
    """One full preview shot. Caller must have checked guard_reason() and must
    serialize calls (STATE.snapshot_busy)."""
    t0 = time.monotonic()
    sid = time.strftime("%Y%m%d_%H%M%S")
    out_host = SNAPSHOT_DIR / sid
    if out_host.exists():  # same-second collision on rapid auto-refresh
        sid = f"{sid}_{int((time.time() % 1) * 1000):03d}"
        out_host = SNAPSHOT_DIR / sid
    out_container = f"/workspace/app/_runtime/snapshot/{sid}"

    proc = await asyncio.create_subprocess_exec(
        "docker", "exec", "-w", "/workspace", docker_runner.CONTAINER,
        BIN_SNAPSHOT,
        "--config", SNAPSHOT_CONFIG_CONTAINER,
        "--out", out_container,
        "--ip", ip,
        "--exposure-us", str(exposure_us),
        "--gain", str(gain),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        out_b, err_b = await asyncio.wait_for(proc.communicate(), timeout=SNAPSHOT_TIMEOUT_S)
    except asyncio.TimeoutError:
        # Killing the docker exec client does not touch the remote tool — a
        # hung jai_snapshot would hold the camera's control channel forever.
        await docker_runner.pkill(SNAPSHOT_TOOL, "TERM")
        await asyncio.sleep(3)
        if await docker_runner.pgrep(SNAPSHOT_TOOL):
            await docker_runner.pkill(SNAPSHOT_TOOL, "KILL")
        proc.kill()
        await proc.wait()
        return SnapshotResult(False, reason=f"超时（>{SNAPSHOT_TIMEOUT_S:.0f}s），已终止容器内 jai_snapshot",
                              elapsed_s=time.monotonic() - t0)

    stdout = out_b.decode(errors="replace")
    stderr = err_b.decode(errors="replace")
    raw_output = (stdout + "\n--- stderr ---\n" + stderr).strip()

    marker = next((ln for ln in reversed(stdout.splitlines()) if ln.startswith("SNAPSHOT: ")), None)
    if marker is None:
        return SnapshotResult(False, reason=f"未见 SNAPSHOT 标记（exit {proc.returncode}）",
                              raw_output=raw_output, elapsed_s=time.monotonic() - t0)
    if marker.startswith("SNAPSHOT: FAIL"):
        reason = marker[len("SNAPSHOT: FAIL "):]
        if reason.strip().startswith("3"):
            reason += "（相机不可达或被其他进程占用）"
        return SnapshotResult(False, reason=reason, raw_output=raw_output,
                              elapsed_s=time.monotonic() - t0)

    camera_dir_container = marker[len("SNAPSHOT: OK "):].strip()
    try:
        camera_dir = to_host(camera_dir_container)
    except ValueError as e:
        return SnapshotResult(False, reason=str(e), raw_output=raw_output,
                              elapsed_s=time.monotonic() - t0)

    # Decode on the host (venv has numpy + opencv): 16-bit color PNG.
    try:
        unpack = await asyncio.to_thread(
            subprocess.run,
            [str(VENV_PYTHON), str(UNPACK_SCRIPT), str(camera_dir),
             "--format", "png", "--demosaic", "--shift-to-16bit",
             "--limit", "1", "--pad-incomplete", "-o", str(out_host)],
            capture_output=True, text=True, timeout=UNPACK_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired as e:
        partial = ((e.stdout or "") + (e.stderr or "")).strip()
        return SnapshotResult(False, reason=f"解码超时（>{UNPACK_TIMEOUT_S:.0f}s）",
                              raw_output=raw_output + "\n--- unpack (timeout) ---\n" + partial,
                              elapsed_s=time.monotonic() - t0)
    raw_output += "\n--- unpack ---\n" + (unpack.stdout + unpack.stderr).strip()
    incomplete = "INCOMPLETE" in unpack.stderr
    pngs = sorted(out_host.glob("seq*_*.png"))
    if unpack.returncode != 0 or not pngs:
        return SnapshotResult(False, reason=f"解码失败: {unpack.stderr.strip()[-300:]}",
                              raw_output=raw_output, incomplete=incomplete,
                              elapsed_s=time.monotonic() - t0)
    png = pngs[0]
    decode_name = png.stem.split("_", 1)[1] if "_" in png.stem else ""

    result = await asyncio.to_thread(_encode_and_histogram, png)
    result.ok = True
    result.decode_name = decode_name
    result.incomplete = incomplete
    result.raw_output = raw_output
    result.elapsed_s = time.monotonic() - t0

    await asyncio.to_thread(_cleanup_old)
    return result


def _encode_and_histogram(png_path) -> SnapshotResult:
    img = cv2.imread(str(png_path), cv2.IMREAD_UNCHANGED)
    if img is None:
        return SnapshotResult(False, reason="无法读取解码后的 PNG")
    if img.dtype == np.uint16:
        img8 = (img >> 8).astype(np.uint8)
        gray16 = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
    else:
        img8 = img
        gray8 = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
        gray16 = gray8.astype(np.uint16) << 8
    ok, jpg = cv2.imencode(".jpg", img8, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
    if not ok:
        return SnapshotResult(False, reason="JPEG 编码失败")
    hist, _ = np.histogram(gray16, bins=64, range=(0, 65536))
    return SnapshotResult(
        ok=False,  # caller flips to True after filling metadata
        jpeg_b64=base64.b64encode(jpg.tobytes()).decode(),
        histogram=[int(v) for v in hist],
        clipped_pct=float((gray16 >= CLIP_THRESHOLD).mean() * 100.0),
        mean_16=float(gray16.mean()),
    )


def _cleanup_old() -> None:
    if not SNAPSHOT_DIR.is_dir():
        return
    dirs = sorted(p for p in SNAPSHOT_DIR.iterdir() if p.is_dir())
    for stale in dirs[:-SNAPSHOT_KEEP]:
        shutil.rmtree(stale, ignore_errors=True)
