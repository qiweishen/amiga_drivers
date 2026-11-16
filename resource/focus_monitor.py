#!/usr/bin/env python3
"""
focus_monitor.py - live focus-sharpness readout for a Siemens-star target.

It follows a camera recording while frames are being written. For each new frame
it takes ONE green plane of the raw Bayer mosaic (no demosaic), crops an ROI
around the star, and computes a normalised sharpness score:

    lapvar    = var(Laplacian(G_roi)) / mean(G_roi)^2      (default)
    tenengrad = mean(Sobel_x^2 + Sobel_y^2) / mean(G_roi)^2

Dividing by mean^2 makes the score insensitive to overall brightness (clouds,
exposure), so it tracks focus and not illumination. The score is smoothed with
a running median over the last N frames. The tool shows:

    current (median) score | session peak | % of peak | ROI saturation warning

How to use it while focusing: set aperture + manual exposure first, then turn
the focus ring slowly THROUGH the peak until % drops clearly, come back until
you're >= 95 %, lock the ring and check that it stays >= 95 % after locking.
Press "Reset peak" (UI) or Ctrl+C + restart (CLI) when you change target
height, aperture or lighting, because the peak is only meaningful within one setup.

------------------------------------------------------------------------------
FRAME SOURCES
------------------------------------------------------------------------------
1) raw  - a binary file of back-to-back frames (e.g. your eBUS logger output)

     [file_header bytes] { [frame_header] [W*H*2 bytes pixels] [frame_footer] } ...

   Pixels are BayerRG12 unpacked in uint16, little-endian (values 0..4095).
   Set --file-header / --frame-header / --frame-footer to match YOUR logger
   format (all default 0). --path may be a glob (e.g. '/data/run_*.bin'): the
   newest file by mtime is followed, so rolling files are handled.

2) images - a directory/glob of single-frame files (.npy .png .tif .tiff .pgm),
   e.g. frames dumped by another tool. Newest file is used.

Live mode (default) always jumps to the NEWEST complete frame, so it never
falls behind a fast recording. --replay processes every frame in order (for
analysing a recorded focus sweep offline) and exits at the end (CLI).

------------------------------------------------------------------------------
EXAMPLES
------------------------------------------------------------------------------
# Web UI (open http://<edge-pc>:8090 from your laptop; click the star to set ROI)
python3 focus_monitor.py raw --path /data/jai/current.bin --width 4128 --height 3008 \
        --frame-header 64 --ui web

# Plain terminal over SSH, ROI centred on the star at full-res pixel (2060, 1500)
python3 focus_monitor.py raw --path /data/jai/current.bin --width 4128 --height 3008 \
        --roi-center 2060 1500 --roi-half 200

# Save a downsampled snapshot with a pixel grid, to find --roi-center headless
python3 focus_monitor.py raw --path /data/jai/current.bin --width 4128 --height 3008 \
        --snapshot snap.jpg

# Offline: score every frame of a recorded sweep, write CSV
python3 focus_monitor.py raw --path sweep.bin --width 4128 --height 3008 \
        --roi-center 2060 1500 --replay --csv sweep_scores.csv

Requires: numpy, opencv-python; nicegui only for --ui web.
"""
from __future__ import annotations

import argparse
import base64
import collections
import glob
import os
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field

import cv2
import numpy as np

# (row, col) of ONE green pixel in the 2x2 Bayer tile
G_OFFSET = {"RG": (0, 1), "BG": (0, 1), "GR": (0, 0), "GB": (0, 0), "MONO": None}


# =============================================================================
# Frame sources
# =============================================================================
def _newest(pattern: str) -> str | None:
    if os.path.isfile(pattern):
        return pattern
    files = [f for f in glob.glob(pattern) if os.path.isfile(f)]
    return max(files, key=os.path.getmtime) if files else None


class RawFileSource:
    """Back-to-back raw frames in a (growing) binary file."""

    def __init__(self, path: str, width: int, height: int, dtype: str = "<u2",
                 file_header: int = 0, frame_header: int = 0, frame_footer: int = 0,
                 replay: bool = False):
        self.pattern, self.w, self.h = path, width, height
        self.dtype = np.dtype(dtype)
        self.file_header, self.frame_header = file_header, frame_header
        self.stride = frame_header + width * height * self.dtype.itemsize + frame_footer
        self.replay = replay
        self.cur_file: str | None = None
        self.next_idx = 0
        self.last_idx = -1
        self.exhausted = False

    def _n_complete(self, path: str) -> int:
        size = os.path.getsize(path)
        return max(0, (size - self.file_header) // self.stride)

    def poll(self):
        """Return (label, frame) for a new frame, or None if nothing new yet."""
        path = self.pattern if self.replay else _newest(self.pattern)
        if path is None or not os.path.exists(path):
            return None
        if path != self.cur_file:
            self.cur_file, self.next_idx, self.last_idx = path, 0, -1
        n = self._n_complete(path)
        if self.replay:
            if self.next_idx >= n:
                self.exhausted = True
                return None
            idx = self.next_idx
            self.next_idx += 1
        else:
            idx = n - 1
            if idx < 0 or idx == self.last_idx:
                return None
        self.last_idx = idx
        off = self.file_header + idx * self.stride + self.frame_header
        frame = np.memmap(path, dtype=self.dtype, mode="r", offset=off, shape=(self.h, self.w))
        return f"{os.path.basename(path)}#{idx}", frame


class ImageFilesSource:
    """Single-frame files (.npy/.png/.tif/.pgm) in a directory or glob."""

    EXT = (".npy", ".png", ".tif", ".tiff", ".pgm")

    def __init__(self, path: str, replay: bool = False):
        self.pattern = os.path.join(path, "*") if os.path.isdir(path) else path
        self.replay = replay
        self.done: set[str] = set()
        self.last: str | None = None
        self.exhausted = False

    def _files(self):
        return [f for f in glob.glob(self.pattern) if f.lower().endswith(self.EXT)]

    def poll(self):
        files = self._files()
        if not files:
            return None
        if self.replay:
            todo = sorted(f for f in files if f not in self.done)
            if not todo:
                self.exhausted = True
                return None
            f = todo[0]
            self.done.add(f)
        else:
            f = max(files, key=os.path.getmtime)
            if f == self.last:
                return None
            # skip a file that is still being written
            if time.time() - os.path.getmtime(f) < 0.2:
                return None
        self.last = f
        img = np.load(f) if f.lower().endswith(".npy") else cv2.imread(f, cv2.IMREAD_UNCHANGED)
        if img is None:
            return None
        if img.ndim == 3:
            img = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        return os.path.basename(f), img


# =============================================================================
# Analysis
# =============================================================================
def green_plane(frame: np.ndarray, pattern: str) -> np.ndarray:
    off = G_OFFSET[pattern]
    return frame if off is None else frame[off[0]::2, off[1]::2]


def sharpness(patch: np.ndarray, method: str) -> float:
    p = np.asarray(patch, dtype=np.float32)
    m = float(p.mean())
    if m < 1e-6:
        return 0.0
    if method == "tenengrad":
        gx = cv2.Sobel(p, cv2.CV_32F, 1, 0, ksize=3)
        gy = cv2.Sobel(p, cv2.CV_32F, 0, 1, ksize=3)
        return float(np.mean(gx * gx + gy * gy)) / (m * m)
    lap = cv2.Laplacian(p, cv2.CV_32F, ksize=3)
    return float(lap.var()) / (m * m)


@dataclass
class Roi:
    cx: int          # full-resolution pixel coordinates
    cy: int
    half: int        # half-size in full-res pixels

    def g_slices(self, g_shape, pattern: str):
        """Slices into the green plane (half resolution for Bayer)."""
        s = 1 if G_OFFSET[pattern] is None else 2
        gh, gw = g_shape
        x0 = int(np.clip((self.cx - self.half) // s, 0, gw - 1))
        x1 = int(np.clip((self.cx + self.half) // s, x0 + 1, gw))
        y0 = int(np.clip((self.cy - self.half) // s, 0, gh - 1))
        y1 = int(np.clip((self.cy + self.half) // s, y0 + 1, gh))
        return slice(y0, y1), slice(x0, x1)


@dataclass
class FocusState:
    median_n: int = 5
    buf: collections.deque = field(default_factory=collections.deque)
    history: collections.deque = field(default_factory=lambda: collections.deque(maxlen=600))
    peak: float | None = None
    current: float | None = None
    raw: float | None = None
    sat_frac: float = 0.0
    label: str = "-"
    n_frames: int = 0
    fps: float = 0.0
    _t_last: float = 0.0

    def push(self, score: float, sat_frac: float, label: str) -> None:
        self.buf.append(score)
        while len(self.buf) > max(1, self.median_n):
            self.buf.popleft()
        self.raw = score
        self.current = statistics.median(self.buf)
        self.peak = self.current if self.peak is None else max(self.peak, self.current)
        self.sat_frac, self.label = sat_frac, label
        self.n_frames += 1
        now = time.time()
        if self._t_last:
            dt = now - self._t_last
            self.fps = 0.8 * self.fps + 0.2 * (1.0 / dt) if self.fps else 1.0 / dt
        self._t_last = now
        self.history.append((now, self.current))

    @property
    def pct(self) -> float | None:
        if self.current is None or not self.peak:
            return None
        return 100.0 * self.current / self.peak

    def reset_peak(self) -> None:
        self.buf.clear()
        self.history.clear()
        self.peak = self.current = self.raw = None


class Analyzer:
    def __init__(self, args):
        self.pattern = args.bayer.upper()
        self.method = args.method
        self.sat_level = args.sat_level
        self.roi: Roi | None = (Roi(args.roi_center[0], args.roi_center[1], args.roi_half)
                                if args.roi_center else None)
        self.roi_half = args.roi_half

    def ensure_roi(self, frame: np.ndarray) -> Roi:
        if self.roi is None:  # default: frame centre
            self.roi = Roi(frame.shape[1] // 2, frame.shape[0] // 2, self.roi_half)
        return self.roi

    def analyze(self, frame: np.ndarray):
        roi = self.ensure_roi(frame)
        g = green_plane(frame, self.pattern)
        ys, xs = roi.g_slices(g.shape, self.pattern)
        patch = np.asarray(g[ys, xs])
        score = sharpness(patch, self.method)
        sat = float(np.mean(patch >= self.sat_level)) if patch.size else 0.0
        return score, sat, g, patch


# =============================================================================
# Visualisation helpers
# =============================================================================
def stretch_u8(img: np.ndarray, lo_pct=0.5, hi_pct=99.5) -> np.ndarray:
    a = np.asarray(img, dtype=np.float32)
    lo, hi = np.percentile(a, [lo_pct, hi_pct])
    if hi <= lo:
        hi = lo + 1
    return np.clip((a - lo) * (255.0 / (hi - lo)), 0, 255).astype(np.uint8)


def overview(g: np.ndarray, roi: Roi, pattern: str, max_w: int = 1000, grid: bool = False):
    """Downsampled green plane with ROI box. Returns (bgr, full_res_px_per_preview_px)."""
    s = 1 if G_OFFSET[pattern] is None else 2
    k = max(1, int(np.ceil(g.shape[1] / max_w)))
    small = stretch_u8(np.asarray(g[::k, ::k]))
    bgr = cv2.cvtColor(small, cv2.COLOR_GRAY2BGR)
    scale = s * k  # full-res pixels per preview pixel
    x0, y0 = (roi.cx - roi.half) / scale, (roi.cy - roi.half) / scale
    x1, y1 = (roi.cx + roi.half) / scale, (roi.cy + roi.half) / scale
    cv2.rectangle(bgr, (int(x0), int(y0)), (int(x1), int(y1)), (0, 200, 255), 2)
    if grid:
        step_full = 500
        for xf in range(0, int(bgr.shape[1] * scale), step_full):
            x = int(xf / scale)
            cv2.line(bgr, (x, 0), (x, bgr.shape[0] - 1), (0, 255, 0), 1)
            cv2.putText(bgr, str(xf), (x + 2, 12), cv2.FONT_HERSHEY_SIMPLEX, 0.35, (0, 255, 0), 1)
        for yf in range(0, int(bgr.shape[0] * scale), step_full):
            y = int(yf / scale)
            cv2.line(bgr, (0, y), (bgr.shape[1] - 1, y), (0, 255, 0), 1)
            cv2.putText(bgr, str(yf), (2, y + 12), cv2.FONT_HERSHEY_SIMPLEX, 0.35, (0, 255, 0), 1)
    return bgr, scale


def to_data_url(bgr: np.ndarray, quality: int = 80) -> str:
    ok, buf = cv2.imencode(".jpg", bgr, [cv2.IMWRITE_JPEG_QUALITY, quality])
    return "data:image/jpeg;base64," + base64.b64encode(buf.tobytes()).decode()


# =============================================================================
# Modes
# =============================================================================
def run_snapshot(src, an: Analyzer, out: str, timeout: float = 10.0) -> None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = src.poll()
        if r:
            label, frame = r
            an.ensure_roi(frame)
            g = green_plane(frame, an.pattern)
            bgr, scale = overview(g, an.roi, an.pattern, max_w=1600, grid=True)
            cv2.imwrite(out, bgr)
            print(f"wrote {out} from {label}  (grid lines every 500 full-res px; "
                  f"frame {frame.shape[1]}x{frame.shape[0]})")
            return
        time.sleep(0.05)
    sys.exit("no frame found")


def run_cli(src, an: Analyzer, st: FocusState, csv_path: str | None, poll_s: float) -> None:
    csv_f = open(csv_path, "w") if csv_path else None
    if csv_f:
        csv_f.write("frame,score,median,peak,pct_of_peak,sat_frac\n")
    replay = getattr(src, "replay", False)
    try:
        while True:
            r = src.poll()
            if r is None:
                if replay and src.exhausted:
                    break
                time.sleep(poll_s)
                continue
            label, frame = r
            score, sat, _, _ = an.analyze(frame)
            st.push(score, sat, label)
            pct = st.pct or 0.0
            bar = "#" * int(pct / 2.5)
            warn = f"  SAT {100 * sat:.1f}%!" if sat > 0.005 else ""
            line = (f"{label:>24s}  score {score:9.4g}  med {st.current:9.4g}  "
                    f"peak {st.peak:9.4g}  {pct:5.1f}% |{bar:<40s}|{warn}")
            if replay:
                print(line)
            else:
                print("\r" + line, end="", flush=True)
            if csv_f:
                csv_f.write(f"{label},{score:.6g},{st.current:.6g},{st.peak:.6g},{pct:.2f},{sat:.5f}\n")
    except KeyboardInterrupt:
        pass
    finally:
        if csv_f:
            csv_f.close()
    print()
    if st.peak is not None:
        print(f"session peak (median-smoothed): {st.peak:.6g}")


def run_web(src, an: Analyzer, st: FocusState, args) -> None:
    from nicegui import ui

    lock = threading.Lock()
    shared = {"overview": None, "overview_scale": 1, "crop": None}
    stop = threading.Event()

    def worker():
        last_preview = 0.0
        while not stop.is_set():
            r = src.poll()
            if r is None:
                time.sleep(args.poll)
                continue
            label, frame = r
            with lock:
                score, sat, g, patch = an.analyze(frame)
                st.push(score, sat, label)
                roi = Roi(an.roi.cx, an.roi.cy, an.roi.half)
            now = time.time()
            crop = stretch_u8(patch)
            crop = cv2.resize(crop, None, fx=2, fy=2, interpolation=cv2.INTER_NEAREST)
            crop_url = to_data_url(cv2.cvtColor(crop, cv2.COLOR_GRAY2BGR), 90)
            ov_url, scale = None, None
            if now - last_preview > args.preview_interval:
                bgr, scale = overview(g, roi, an.pattern, max_w=1000)
                ov_url = to_data_url(bgr)
                last_preview = now
            with lock:
                shared["crop"] = crop_url
                if ov_url:
                    shared["overview"], shared["overview_scale"] = ov_url, scale

    threading.Thread(target=worker, daemon=True).start()

    @ui.page("/")
    def index():
        ui.label("Focus monitor").classes("text-2xl font-bold")
        with ui.row().classes("items-end gap-10"):
            with ui.column().classes("gap-0"):
                ui.label("% of peak").classes("text-sm text-gray-500")
                pct_lbl = ui.label("-").classes("text-7xl font-bold")
            with ui.column().classes("gap-0"):
                ui.label("current (median)").classes("text-sm text-gray-500")
                cur_lbl = ui.label("-").classes("text-3xl font-mono")
            with ui.column().classes("gap-0"):
                ui.label("session peak").classes("text-sm text-gray-500")
                peak_lbl = ui.label("-").classes("text-3xl font-mono")
            with ui.column().classes("gap-0"):
                ui.label("frame / fps").classes("text-sm text-gray-500")
                info_lbl = ui.label("-").classes("text-base font-mono")
        sat_lbl = ui.label("").classes("text-lg font-bold text-red-600")

        with ui.row().classes("items-center gap-4"):
            ui.button("Reset peak", on_click=lambda: _reset()).props("color=primary")
            half_in = ui.number("ROI half-size (full-res px)", value=an.roi_half, min=32, max=1500,
                                step=16, on_change=lambda e: _set_half(e.value)).classes("w-56")
            med_in = ui.number("median over N frames", value=st.median_n, min=1, max=50, step=1,
                               on_change=lambda e: _set_med(e.value)).classes("w-48")
            ui.select({"lapvar": "Laplacian variance", "tenengrad": "Tenengrad"},
                      value=an.method, label="metric",
                      on_change=lambda e: _set_method(e.value)).classes("w-56")
            ui.label("Click the star centre in the overview to move the ROI.").classes("text-gray-500")

        chart = ui.echart({
            "animation": False,
            "grid": {"left": 60, "right": 20, "top": 20, "bottom": 30},
            "xAxis": {"type": "value", "name": "s", "min": "dataMin"},
            "yAxis": {"type": "value", "scale": True},
            "series": [
                {"type": "line", "name": "median score", "showSymbol": False, "data": []},
                {"type": "line", "name": "peak", "showSymbol": False, "data": [],
                 "lineStyle": {"type": "dashed"}},
            ],
        }).classes("w-full h-56")

        with ui.row().classes("w-full items-start gap-4 no-wrap"):
            with ui.column():
                ui.label("Overview (green plane, ROI in orange)").classes("text-sm text-gray-500")
                ov_img = ui.interactive_image(on_mouse=lambda e: _click(e), events=["click"]) \
                    .classes("w-[1000px] max-w-full")
            with ui.column():
                ui.label("ROI, green plane, 2x nearest").classes("text-sm text-gray-500")
                crop_img = ui.interactive_image().classes("w-[420px]")

        def _reset():
            with lock:
                st.reset_peak()

        def _set_half(v):
            if v:
                with lock:
                    an.roi_half = int(v)
                    if an.roi:
                        an.roi.half = int(v)
                    st.reset_peak()

        def _set_med(v):
            if v:
                with lock:
                    st.median_n = int(v)

        def _set_method(v):
            with lock:
                an.method = v
                st.reset_peak()

        def _click(e):
            with lock:
                scale = shared["overview_scale"]
                if an.roi is None:
                    return
                an.roi.cx = int(e.image_x * scale)
                an.roi.cy = int(e.image_y * scale)
                st.reset_peak()

        state = {"ov": None, "crop": None}

        def refresh():
            with lock:
                pct, cur, peak = st.pct, st.current, st.peak
                sat, label, fps = st.sat_frac, st.label, st.fps
                hist = list(st.history)
                ov, crop = shared["overview"], shared["crop"]
            if pct is None:
                pct_lbl.text = "-"
            else:
                pct_lbl.text = f"{pct:5.1f}%"
                color = ("text-green-600" if pct >= 95 else
                         "text-amber-500" if pct >= 80 else "text-red-600")
                pct_lbl.classes(replace=f"text-7xl font-bold {color}")
            cur_lbl.text = "-" if cur is None else f"{cur:.5g}"
            peak_lbl.text = "-" if peak is None else f"{peak:.5g}"
            info_lbl.text = f"{label}  {fps:4.1f} fps"
            sat_lbl.text = (f"ROI saturated: {100 * sat:.1f}% of pixels >= {an.sat_level} "
                            "- reduce exposure, clipped edges inflate the score") if sat > 0.005 else ""
            if hist:
                t0 = hist[0][0]
                chart.options["series"][0]["data"] = [[round(t - t0, 2), v] for t, v in hist]
                chart.options["series"][1]["data"] = (
                    [[0, peak], [round(hist[-1][0] - t0, 2), peak]] if peak else [])
            else:
                chart.options["series"][0]["data"] = []
                chart.options["series"][1]["data"] = []
            chart.update()
            if ov and ov is not state["ov"]:
                ov_img.set_source(ov)
                state["ov"] = ov
            if crop and crop is not state["crop"]:
                crop_img.set_source(crop)
                state["crop"] = crop

        ui.timer(args.ui_interval, refresh)

    ui.run(host=args.host, port=args.port, title="Focus monitor", reload=False, show=False,
           show_welcome_message=True)


# =============================================================================
# CLI
# =============================================================================
def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="source", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--path", required=True, help="file, directory or glob")
    common.add_argument("--bayer", default="RG", choices=["RG", "GR", "GB", "BG", "MONO", "rg", "gr", "gb", "bg", "mono"],
                        help="Bayer pattern of the raw data (default RG = BayerRG)")
    common.add_argument("--roi-center", type=int, nargs=2, metavar=("X", "Y"),
                        help="ROI centre in full-res pixels (default: frame centre; click in web UI)")
    common.add_argument("--roi-half", type=int, default=200, help="ROI half-size, full-res px (default 200)")
    common.add_argument("--method", default="lapvar", choices=["lapvar", "tenengrad"])
    common.add_argument("--median", type=int, default=5, help="running median over N frames (default 5)")
    common.add_argument("--sat-level", type=int, default=4095,
                        help="pixel value counted as saturated (default 4095 for 12-bit LSB-aligned)")
    common.add_argument("--replay", action="store_true", help="process every frame in order, then exit (CLI)")
    common.add_argument("--csv", help="CLI: write per-frame scores to CSV")
    common.add_argument("--snapshot", help="save one overview JPEG with a pixel grid and exit")
    common.add_argument("--ui", choices=["cli", "web"], default="cli")
    common.add_argument("--host", default="0.0.0.0")
    common.add_argument("--port", type=int, default=8090)
    common.add_argument("--poll", type=float, default=0.05, help="poll interval, s")
    common.add_argument("--ui-interval", type=float, default=0.25, help="web UI refresh, s")
    common.add_argument("--preview-interval", type=float, default=1.0, help="overview refresh, s")

    p_raw = sub.add_parser("raw", parents=[common], help="back-to-back raw frames in a binary file")
    p_raw.add_argument("--width", type=int, required=True)
    p_raw.add_argument("--height", type=int, required=True)
    p_raw.add_argument("--dtype", default="<u2", help="numpy dtype of a pixel (default <u2)")
    p_raw.add_argument("--file-header", type=int, default=0, help="bytes before the first frame")
    p_raw.add_argument("--frame-header", type=int, default=0, help="bytes before each frame's pixels")
    p_raw.add_argument("--frame-footer", type=int, default=0, help="bytes after each frame's pixels")

    sub.add_parser("images", parents=[common], help="single-frame image/npy files")
    return ap


def main() -> None:
    args = build_parser().parse_args()
    args.bayer = args.bayer.upper()
    if args.source == "raw":
        src = RawFileSource(args.path, args.width, args.height, args.dtype,
                            args.file_header, args.frame_header, args.frame_footer, replay=args.replay)
    else:
        src = ImageFilesSource(args.path, replay=args.replay)
    an = Analyzer(args)
    st = FocusState(median_n=args.median)

    if args.snapshot:
        run_snapshot(src, an, args.snapshot)
    elif args.ui == "web":
        run_web(src, an, st, args)
    else:
        run_cli(src, an, st, args.csv, args.poll)


if __name__ == "__main__":
    main()
