"""On-demand preview of the RUNNING acquisition session (Data Live page).

Nothing polls: each fetch is a single read of the files the drivers are
appending to right now, restricted to committed bytes.

- GoX: newest frame from the live jai-raw-seg segment — idx.jsonl tail gives
  the record offset, the payload sits at off + 96 (frame header), decode via
  gox_driver/scripts/unpack_raw.py (PFNC table + demosaic).
- FX10: image-row statistics over committed BIL lines, using capture.json
  geometry and the line index. SensorSync edge observations require a verified
  association before they can be treated as GNSS camera-line timestamps.
"""

from __future__ import annotations

import base64
import asyncio
import csv
import importlib.util
import json
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np

from ..constants import UNPACK_SCRIPT
from ..state import STATE, ProcState

_GOX_FRAME_HEADER_BYTES = 96  # jai-raw-seg FrameHeader (frozen format)
_IDX_TAIL_BYTES = 256 * 1024  # more than enough for the last idx.jsonl lines
_WINDOW_S = 1.0  # "the last second before the click"
_FX10_STALE_S = 3.0  # .bil.part mtime older than this = the lines stopped


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
    request: PreviewRequest | None = None


@dataclass
class Fx10Live:
    ok: bool
    reason: str = ""
    wavelengths_nm: list[float] = field(default_factory=list)
    spectrum_pct: dict[str, list[float]] = field(default_factory=dict)
    frames: int = 0
    bands: int = 0
    samples: int = 0
    age_s: float = 0.0  # click time minus the .bil.part mtime (newest line)
    request: PreviewRequest | None = None


# --- shared ------------------------------------------------------------------

@dataclass(frozen=True)
class PreviewRequest:
    session: Path
    generation: int


class PreviewUnavailable(RuntimeError):
    pass


class PreviewObsolete(PreviewUnavailable):
    pass


_preview_task: asyncio.Task | None = None


def busy() -> bool:
    return _preview_task is not None and not _preview_task.done()


def is_current(request: PreviewRequest | None) -> bool:
    return (request is not None and STATE.process_state is ProcState.RUNNING
            and STATE.ownership_verified and not STATE.control_uncertain
            and request.generation == STATE.session_generation
            and request.session == STATE.active_session)


async def _fetch(driver: str, decode):
    global _preview_task
    if busy():
        raise PreviewUnavailable("Another live preview is still being decoded; wait for it to finish")
    if (STATE.process_state is not ProcState.RUNNING or STATE.active_session is None
            or not STATE.ownership_verified or STATE.control_uncertain):
        raise PreviewUnavailable("There is no verified running session to preview")
    if not STATE.enables_at_start.get(driver, False):
        raise PreviewUnavailable(f"{driver} is not enabled in this recording")
    request = PreviewRequest(STATE.active_session, STATE.session_generation)

    async def decode_request():
        # Pass a fixed path into the worker. It must never switch sessions by
        # rereading global UI state halfway through a file operation.
        result = await asyncio.to_thread(decode, request.session)
        result.request = request
        return result

    _preview_task = asyncio.create_task(decode_request())
    _preview_task.add_done_callback(lambda task: None if task.cancelled() else task.exception())
    # Cancelling a page await does not stop a worker thread. Keep the global
    # reservation until that worker really finishes; do not queue more work.
    result = await asyncio.shield(_preview_task)
    if not is_current(request):
        raise PreviewObsolete("The recording changed while the preview was being decoded")
    return result


async def fetch_gox() -> GoxLive:
    return await _fetch("gox", gox_latest_frame)


async def fetch_fx10() -> Fx10Live:
    return await _fetch("fx10", fx10_spectrum)


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


def gox_latest_frame(session: Path) -> GoxLive:
    """Newest committed frame of the live GoX session, demosaiced to JPEG.

    The idx.jsonl writer buffers up to ~1 s / 100 frames, so the newest entry
    can lag the sensor by up to a second — exactly the requested window."""
    root = session / "raw" / "gox"
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
        if int(rec.get("fl", 0)) & 3:
            return GoxLive(False, reason="Newest frame is incomplete or has an SDK receive error")
        if any(key not in rec for key in ("padding_x", "padding_y", "chunk_count")):
            return GoxLive(False, reason="Frame layout metadata is unavailable; refusing to guess row stride")
        if any(int(rec[key]) != 0 for key in ("padding_x", "padding_y", "chunk_count")):
            return GoxLive(False, reason="Preview does not support this padded/chunked payload; raw data is retained")
        if not 0 < int(rec["psz"]) <= 128 * 1024 * 1024 or int(rec["off"]) < 0:
            return GoxLive(False, reason="Frame size or offset exceeds preview bounds")
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

def _fx10_geometry(capture_dir: Path) -> tuple[int, int, int, list[float], float] | str:
    """Use immutable acquisition metadata, never the editable requested config."""
    try:
        doc = json.loads((capture_dir / "capture.json").read_text(encoding="utf-8"))
        if doc.get("format") != "fx10-capture-v1" or doc.get("byte_order") != "little":
            return "Unsupported capture metadata; refusing to guess the image layout"
        samples, bands = int(doc["samples"]), int(doc["bands"])
        bpp, full_scale = int(doc["bytes_per_pixel"]), int(doc["full_scale"])
        if not (0 < samples <= 65535 and 0 < bands <= 65535):
            return "Invalid capture geometry"
        if (bpp, full_scale) not in ((1, 255), (2, 1023), (2, 4095)):
            return "Unknown capture bit depth"
        frame_rate_hz = float(doc["expected_frame_rate_hz"])
        if not np.isfinite(frame_rate_hz) or not 0 <= frame_rate_hz <= 100000:
            return "Invalid capture frame rate"
        wavelengths = [float(v) for v in doc.get("wavelengths_nm", [])]
        if wavelengths and (len(wavelengths) != bands or not all(np.isfinite(v) and v > 0 for v in wavelengths)):
            return "Invalid capture wavelength axis"
        return samples, bands, full_scale, wavelengths, frame_rate_hz
    except Exception as e:
        return f"Cannot read recorded capture geometry: {e}"


def _fx10_frame_offsets(bil: Path, line_bytes: int, limit: int) -> list[int]:
    """Complete index records are the live commit boundary; skip padding/anomalies."""
    base = bil.name.removesuffix(".part").removesuffix(".bil")
    index = bil.parent / (base + ".lines.csv.part")
    if not index.exists():
        index = bil.parent / (base + ".lines.csv")
    with index.open("rb") as f:
        f.seek(0, 2)
        size = f.tell()
        start = max(0, size - _IDX_TAIL_BYTES)
        f.seek(start)
        raw = f.read()
    if start:
        raw = raw.partition(b"\n")[2]  # discard a potentially partial first record
    raw = raw[:raw.rfind(b"\n") + 1]  # discard an uncommitted trailing record
    offsets = []
    for row in csv.reader(raw.decode("ascii").splitlines()):
        # v2 appends twelve SDK metadata columns; the first ten keep v1 semantics.
        if len(row) in (10, 22) and row[0] == "frame" and row[9] == "0":
            offset = int(row[3])
            if offset >= 0 and offset == int(row[1]) * line_bytes:
                offsets.append(offset)
    return offsets[-limit:]


def fx10_spectrum(session: Path) -> Fx10Live:
    """Per-band statistics over roughly the last second of recorded lines
    (frame_rate_hz worth of lines at the tail of the open segment; same
    reductions and normalization as the Camera Tools snapshot preview)."""
    root = session / "raw" / "fx10"
    session_dirs = sorted(d for d in root.iterdir() if d.is_dir()) if root.is_dir() else []
    if not session_dirs:
        return Fx10Live(False, reason=f"No FX10 session under {root} yet")
    # The OPEN segment carries a .part suffix (renamed on finalize); prefer it,
    # fall back to the newest finalized segment right after a rotation.
    bil_files = sorted(session_dirs[-1].glob("segment_*.bil.part")) \
        or sorted(session_dirs[-1].glob("segment_*.bil"))
    if not bil_files:
        return Fx10Live(False, reason="No open segment yet")
    bil = bil_files[-1]

    geometry = _fx10_geometry(bil.parent)
    if isinstance(geometry, str):
        return Fx10Live(False, reason=geometry)
    samples, bands, full_scale, wavelengths, frame_rate_hz = geometry
    dtype = np.dtype("u1") if full_scale == 255 else np.dtype("<u2")
    line_bytes = samples * bands * dtype.itemsize

    try:
        stat = bil.stat()
    except OSError as e:
        return Fx10Live(False, reason=f"Cannot stat {bil.name}: {e}")
    if stat.st_size < line_bytes:
        return Fx10Live(False, reason="No complete line on disk yet — retry")

    # The writer appends unbuffered, so the mtime is the newest line's time.
    age = max(0.0, time.time() - stat.st_mtime)
    if age > _FX10_STALE_S:
        return Fx10Live(False, reason=f"No frames in the last {_WINDOW_S:.0f} s "
                                      f"(newest line is {age:.1f} s old — trigger pulses missing?)")

    # Bound preview allocation independently of requested sensor rates/geometry.
    if line_bytes > 64 * 1024 * 1024:
        return Fx10Live(False, reason="Recorded line exceeds the preview memory budget")
    window = min(max(1, round(frame_rate_hz * _WINDOW_S)), max(1, (64 * 1024 * 1024) // line_bytes))
    try:
        offsets = _fx10_frame_offsets(bil, line_bytes, window)
        frames = []
        with bil.open("rb") as f:
            for offset in offsets:
                f.seek(offset)
                frame = f.read(line_bytes)
                if len(frame) == line_bytes:
                    frames.append(frame)
        data = b"".join(frames)
    except (OSError, ValueError, UnicodeError) as e:
        return Fx10Live(False, reason=f"Cannot read committed frame index (retry after rotation): {e}")
    count = len(data) // line_bytes
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
        age_s=age,
    )
