"""On-demand preview of the RUNNING acquisition session (Data Live page).

Nothing polls: each fetch is a single read of the files the drivers are
appending to right now, restricted to committed bytes.

- GoX: newest frame from the live jai-raw-seg segment — idx.jsonl tail gives
  the record offset, the payload sits at off + 96 (frame header), decode via
  gox_driver/scripts/unpack_raw.py (PFNC table + demosaic).
- FX10: per-band spectral statistics over the BIL lines of the last second,
  selected through the fixed-width .times sidecar (one 48-byte record per
  line, written unbuffered — always fresh).
"""

from __future__ import annotations

import base64
import importlib.util
import json
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np
import yaml

from ..constants import CONFIG_FILES, UNPACK_SCRIPT
from ..state import STATE, ProcState
from .fx10_tools import _PIXEL_FULL_SCALE

_GOX_FRAME_HEADER_BYTES = 96  # jai-raw-seg FrameHeader (frozen format)
_IDX_TAIL_BYTES = 256 * 1024  # more than enough for the last idx.jsonl lines
_WINDOW_S = 1.0  # "the last second before the click"

# .times sidecar layout (fx10_driver/include/timestamp_sidecar.hpp)
_SIDECAR_MAGIC = b"FX10TS01"
_SIDECAR_HEADER_BYTES = 64
_SIDECAR_RECORD = np.dtype([
    ("bid", "<u8"), ("hrt", "<u8"), ("hmn", "<u8"), ("dts", "<u8"),
    ("gli", "<u8"), ("flags", "<u4"), ("gap", "<u4"),
])


@dataclass
class GoxLive:
    ok: bool
    reason: str = ""
    jpeg_b64: str = ""
    camera: str = ""
    seq: int = 0
    width: int = 0
    height: int = 0
    pixel_format: str = ""
    age_s: float = 0.0  # click time minus the frame's host timestamp


@dataclass
class Fx10Live:
    ok: bool
    reason: str = ""
    wavelengths_nm: list[float] = field(default_factory=list)
    spectrum_pct: dict[str, list[float]] = field(default_factory=dict)
    frames: int = 0
    bands: int = 0
    samples: int = 0
    age_s: float = 0.0  # click time minus the newest used line's host timestamp


# --- shared ------------------------------------------------------------------

def _session_or_reason() -> tuple[Path | None, str]:
    if STATE.process_state != ProcState.RUNNING:
        return None, "Recording is not running"
    if STATE.active_session is None:
        return None, "No active session directory"
    return STATE.active_session, ""


_unpack_mod = None


def _unpack():
    """gox_driver/scripts/unpack_raw.py as a module (PFNC table, decode,
    demosaic). Loaded once; the scripts dir joins sys.path for its sibling
    `import inspect_raw`."""
    global _unpack_mod
    if _unpack_mod is None:
        scripts_dir = str(UNPACK_SCRIPT.parent)
        if scripts_dir not in sys.path:
            sys.path.insert(0, scripts_dir)
        spec = importlib.util.spec_from_file_location("gox_unpack_raw", UNPACK_SCRIPT)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _unpack_mod = mod
    return _unpack_mod


# --- GoX ---------------------------------------------------------------------

def _idx_tail_records(idx_path: Path) -> list[dict]:
    with idx_path.open("rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - _IDX_TAIL_BYTES))
        tail = f.read().decode(errors="replace")
    records = []
    for line in tail.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                continue  # torn first/partial line of the tail window
    return records


def gox_latest_frame() -> GoxLive:
    """Newest committed frame of the live GoX session, demosaiced to JPEG.

    The idx.jsonl writer buffers up to ~1 s / 100 frames, so the newest entry
    can lag the sensor by up to a second — exactly the requested window."""
    session, reason = _session_or_reason()
    if reason:
        return GoxLive(False, reason=reason)
    if not STATE.enables_at_start.get("gox", False):
        return GoxLive(False, reason="GoX is not enabled in this run")
    root = session / "bin" / "gox"
    cameras = sorted(d for d in root.iterdir() if d.is_dir()) if root.is_dir() else []
    for cam_dir in cameras:
        idx_files = sorted(cam_dir.glob("seg_*.idx.jsonl"))
        if not idx_files:
            continue
        records = _idx_tail_records(idx_files[-1])
        if not records:
            return GoxLive(False, reason=f"No committed frames yet in {idx_files[-1].name} "
                                         "(the index flushes about once per second — retry)")
        rec = records[-1]
        seg = idx_files[-1].with_name(idx_files[-1].name.replace(".idx.jsonl", ".raw"))
        try:
            with seg.open("rb") as f:
                f.seek(int(rec["off"]) + _GOX_FRAME_HEADER_BYTES)
                payload = f.read(int(rec["psz"]))
        except OSError as e:
            return GoxLive(False, reason=f"Cannot read {seg.name}: {e}")
        if len(payload) < int(rec["psz"]):
            return GoxLive(False, reason="Newest frame is not fully on disk yet — retry")

        up = _unpack()
        try:
            img, name, pattern = up.decode(int(rec["pf"]), int(rec["w"]), int(rec["h"]), payload)
        except ValueError as e:
            return GoxLive(False, reason=str(e))
        if pattern in up.CV_BAYER:
            img = up.demosaic(img, pattern)
        # Bit depth from the PFNC name; 12/10-bit data lives in uint16.
        full_scale = 4095 if "12" in name else 1023 if "10" in name else 255
        img8 = np.clip(img.astype(np.float32) / full_scale * 255.0, 0, 255).astype(np.uint8)
        ok, jpg = cv2.imencode(".jpg", img8, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
        if not ok:
            return GoxLive(False, reason="JPEG encoding failed")
        return GoxLive(
            ok=True,
            jpeg_b64=base64.b64encode(jpg.tobytes()).decode(),
            camera=cam_dir.name,
            seq=int(rec.get("seq", 0)),
            width=int(rec["w"]),
            height=int(rec["h"]),
            pixel_format=name,
            age_s=max(0.0, time.time() - int(rec.get("hrt", 0)) / 1e9),
        )
    return GoxLive(False, reason=f"No camera data under {root} yet")


# --- FX10 --------------------------------------------------------------------

def _fx10_geometry() -> tuple[int, int, int, list[float]] | str:
    """(samples, bands, full_scale, wavelengths) from config-fx10.yaml, or an
    error string. The live segment has no .hdr until it is finalized, so the
    config is the only geometry source."""
    try:
        doc = yaml.safe_load(CONFIG_FILES["fx10"].path.read_text(encoding="utf-8")) or {}
    except Exception as e:
        return f"Cannot read config-fx10.yaml: {e}"
    acq = doc.get("acquisition") or {}
    if (acq.get("mroi") or {}).get("enabled", False):
        return "MROI is enabled — the live preview does not support MROI geometry"
    try:
        samples = 1024 // int(acq.get("spatial_binning", 1))
        bands = 448 // int(acq.get("spectral_binning", 1))
    except (TypeError, ValueError, ZeroDivisionError):
        return "Bad spatial/spectral_binning in config-fx10.yaml"
    full_scale = _PIXEL_FULL_SCALE.get(str(acq.get("pixel_format", "Mono12Packed")), 4095)
    wl_cfg = (doc.get("output") or {}).get("wavelengths") or {}
    values = wl_cfg.get("list") or []
    if isinstance(values, list) and len(values) == bands:
        wavelengths = [float(v) for v in values]
    else:
        grid = wl_cfg.get("grid") or {}
        wavelengths = list(np.linspace(float(grid.get("start_nm", 400.0)),
                                       float(grid.get("end_nm", 1000.0)), bands))
    return samples, bands, full_scale, wavelengths


def fx10_spectrum() -> Fx10Live:
    """Per-band statistics over the lines recorded in the last second of the
    live FX10 session (same reductions and normalization as the FX10 Tools
    snapshot preview)."""
    session, reason = _session_or_reason()
    if reason:
        return Fx10Live(False, reason=reason)
    if not STATE.enables_at_start.get("fx10", False):
        return Fx10Live(False, reason="FX10 is not enabled in this run")
    root = session / "bin" / "fx10"
    session_dirs = sorted(d for d in root.iterdir() if d.is_dir()) if root.is_dir() else []
    if not session_dirs:
        return Fx10Live(False, reason=f"No FX10 session under {root} yet")
    # The OPEN segment carries a .part suffix (renamed on finalize); prefer it,
    # fall back to the newest finalized segment right after a rotation.
    times_files = sorted(session_dirs[-1].glob("segment_*.times.part")) \
        or sorted(session_dirs[-1].glob("segment_*.times"))
    if not times_files:
        return Fx10Live(False, reason="No open segment yet")
    times = times_files[-1]

    geometry = _fx10_geometry()
    if isinstance(geometry, str):
        return Fx10Live(False, reason=geometry)
    samples, bands, full_scale, wavelengths = geometry
    dtype = np.uint8 if full_scale == 255 else np.dtype("<u2")
    line_bytes = samples * bands * dtype.itemsize

    raw = times.read_bytes()
    if len(raw) < _SIDECAR_HEADER_BYTES:
        return Fx10Live(False, reason="Sidecar header not on disk yet — retry")
    magic, _version, record_size = struct.unpack_from("<8sII", raw, 0)
    if magic != _SIDECAR_MAGIC or record_size != _SIDECAR_RECORD.itemsize:
        return Fx10Live(False, reason=f"Unexpected sidecar layout in {times.name}")
    n_records = (len(raw) - _SIDECAR_HEADER_BYTES) // _SIDECAR_RECORD.itemsize
    if n_records == 0:
        return Fx10Live(False, reason="No lines recorded in this segment yet")
    records = np.frombuffer(raw, dtype=_SIDECAR_RECORD, count=n_records,
                            offset=_SIDECAR_HEADER_BYTES)

    bil = times.with_name(times.name.replace(".times", ".bil"))  # keeps a .part suffix
    try:
        bil_lines = bil.stat().st_size // line_bytes
    except OSError as e:
        return Fx10Live(False, reason=f"Cannot stat {bil.name}: {e}")
    usable = min(n_records, bil_lines)
    if usable == 0:
        return Fx10Live(False, reason="No complete line on disk yet — retry")

    # Lines whose host timestamp falls inside the last second (timestamps are
    # monotonic per segment, so the selection is a contiguous tail).
    cutoff_ns = int((time.time() - _WINDOW_S) * 1e9)
    hrt = records["hrt"][:usable]
    selected = np.nonzero(hrt >= cutoff_ns)[0]
    if selected.size == 0:
        age = time.time() - float(hrt[usable - 1]) / 1e9
        return Fx10Live(False, reason=f"No frames in the last {_WINDOW_S:.0f} s "
                                      f"(newest line is {age:.1f} s old — trigger pulses missing?)")
    first = int(selected[0])
    count = usable - first

    with bil.open("rb") as f:
        f.seek(first * line_bytes)
        data = f.read(count * line_bytes)
    count = len(data) // line_bytes  # tolerate a torn trailing line
    if count == 0:
        return Fx10Live(False, reason="No complete line on disk yet — retry")
    cube = np.frombuffer(data, dtype=dtype, count=count * bands * samples)
    cube = cube.reshape(count, bands, samples)

    band_axis = (0, 2)
    scale = 100.0 / full_scale
    spectrum_pct = {
        "mean": (cube.mean(axis=band_axis) * scale),
        "median": (np.median(cube, axis=band_axis) * scale),
        "max": (cube.max(axis=band_axis) * scale),
        "min": (cube.min(axis=band_axis) * scale),
        "p90": (np.percentile(cube, 90, axis=band_axis) * scale),
    }
    return Fx10Live(
        ok=True,
        wavelengths_nm=wavelengths,
        spectrum_pct={k: [round(float(x), 3) for x in v] for k, v in spectrum_pct.items()},
        frames=count,
        bands=bands,
        samples=samples,
        age_s=max(0.0, time.time() - float(hrt[first + count - 1]) / 1e9),
    )
