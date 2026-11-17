#!/usr/bin/env python3
"""
fx10e_focus_monitor.py - live focus readout for the Specim FX10e on the static edge target
(make_fx10e_focus_target.py).

Follows the binary file written by the eBUS / GigE Vision logger while it is recording. Each
FX10e frame is one line of the scene: a 2-D array of spatial x spectral pixels. Camera and
target are stationary, so consecutive frames image the same line and are averaged.

Pipeline (each update)
  1. Read the newest N complete frames, average them           -> cube[band, spatial]
  2. Bin bands into groups (default 8 bands, ~20 nm)           -> profile per band group
  3. Detect edges on the band-mean profile inside the ROI. Only isolated edges with flat
     plateaus on both sides are kept, so the bar ladder and the V slit-locator are rejected
     automatically and only the edge-array edges remain.
  4. For every band group: refine each edge centre (50 % crossing), normalise each edge to
     0..1 with its own plateaus, stack all edges into one oversampled ESF and fit a Gaussian
     ESF  ->  LSF FWHM (px).  MTF50 = 0.1874 / sigma (cycles/px, Gaussian model).
  5. Overall focus metric = fit over all edges and all band groups inside --wl-range.
     Zones: the same fit per field-of-view zone (default 3 = L / C / R).

Lower FWHM = sharper.  "% of best" = 100 * session_best_FWHM / current_FWHM.
The FWHM is the system LSF including pixel sampling, so ~1.0-1.3 px is about the floor.
Usable range: edges must be sharper than ~--half-window px; do coarse focus by eye first.

------------------------------------------------------------------------------------------
FRAME FORMAT (adjust to your logger)
------------------------------------------------------------------------------------------
  [file_header bytes] { [frame_header] [frame pixels] [frame_footer] } ...

  --width/--height   frame size as delivered by the camera (e.g. 1024 x 224)
  --spectral-axis    'rows' (default: height = bands) or 'cols'
  --pixel-format     u16           12-bit in uint16, LSB aligned (Mono12 / Mono16)
                     mono12p       PFNC Mono12p (2 px in 3 bytes, LSB first)
                     mono12packed  GigE Vision Mono12Packed
  --wavelengths      text / CSV / ENVI .hdr with one wavelength per band; otherwise a linear
                     map from --wl-first to --wl-last (approximate)
  --path may be a glob ('/data/fx10e/*.bin'): the newest file is followed.

------------------------------------------------------------------------------------------
EXAMPLES
------------------------------------------------------------------------------------------
# Web UI (open http://<edge-pc>:8091)
python3 fx10e_focus_monitor.py --path '/data/fx10e/*.bin' --width 1024 --height 224 \
        --frame-header 64 --ui web

# Terminal over SSH
python3 fx10e_focus_monitor.py --path /data/fx10e/run.bin --width 1024 --height 224

# Offline: score a recorded focus sweep, per-band FWHM to CSV
python3 fx10e_focus_monitor.py --path sweep.bin --width 1024 --height 224 --replay --csv sweep.csv

Requires: numpy; nicegui only for --ui web.
"""
from __future__ import annotations

import argparse
import collections
import glob
import math
import os
import re
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field

import numpy as np

FWHM_PER_SIGMA = 2.0 * math.sqrt(2.0 * math.log(2.0))   # 2.3548
MTF50_SIGMA = math.sqrt(math.log(2.0) / 2.0) / math.pi   # MTF50 = 0.1874 / sigma


# =============================================================================
# Frame source
# =============================================================================
def _newest(pattern: str) -> str | None:
    if os.path.isfile(pattern):
        return pattern
    files = [f for f in glob.glob(pattern) if os.path.isfile(f)]
    return max(files, key=os.path.getmtime) if files else None


def frame_nbytes(width: int, height: int, fmt: str) -> int:
    n = width * height
    if fmt == "u16":
        return 2 * n
    if n % 2:
        raise ValueError("packed 12-bit formats need an even pixel count")
    return n * 3 // 2


def unpack(raw: np.ndarray, fmt: str, height: int, width: int) -> np.ndarray:
    """raw: (k, frame_bytes) uint8 -> (k, height, width) uint16."""
    k = raw.shape[0]
    if fmt == "u16":
        return np.ascontiguousarray(raw).view("<u2").reshape(k, height, width)
    b = raw.reshape(k, -1, 3).astype(np.uint16)
    if fmt == "mono12p":          # PFNC: p0 = b0 | (b1 & 0xF) << 8 ; p1 = b1 >> 4 | b2 << 4
        p0 = b[..., 0] | ((b[..., 1] & 0x0F) << 8)
        p1 = (b[..., 1] >> 4) | (b[..., 2] << 4)
    elif fmt == "mono12packed":   # GigE Vision: p0 = b0 << 4 | (b1 & 0xF) ; p1 = b2 << 4 | b1 >> 4
        p0 = (b[..., 0] << 4) | (b[..., 1] & 0x0F)
        p1 = (b[..., 2] << 4) | (b[..., 1] >> 4)
    else:
        raise ValueError(fmt)
    return np.stack([p0, p1], axis=-1).reshape(k, height, width)


class RawFrameSource:
    """Back-to-back frames in a (growing) binary file. Returns blocks of the newest N frames."""

    def __init__(self, path, width, height, fmt="u16", file_header=0, frame_header=0,
                 frame_footer=0, replay=False):
        self.pattern, self.w, self.h, self.fmt = path, width, height, fmt
        self.file_header, self.frame_header = file_header, frame_header
        self.fbytes = frame_nbytes(width, height, fmt)
        self.stride = frame_header + self.fbytes + frame_footer
        self.replay = replay
        self.cur_file = None
        self.next_idx = 0
        self.last_idx = -1
        self.exhausted = False

    def poll_block(self, n: int):
        """Return (label, frames[k, h, w] uint16) or None if nothing new."""
        path = self.pattern if self.replay else _newest(self.pattern)
        if path is None or not os.path.exists(path):
            return None
        if path != self.cur_file:
            self.cur_file, self.next_idx, self.last_idx = path, 0, -1
        n_complete = max(0, (os.path.getsize(path) - self.file_header) // self.stride)
        if self.replay:
            if self.next_idx + n > n_complete:
                self.exhausted = True
                return None
            first, last = self.next_idx, self.next_idx + n - 1
            self.next_idx += n
        else:
            last = n_complete - 1
            if last < 0 or last == self.last_idx:
                return None
            first = max(0, last - n + 1)
        self.last_idx = last
        k = last - first + 1
        off = self.file_header + first * self.stride
        mm_ = np.memmap(path, dtype=np.uint8, mode="r", offset=off, shape=(k * self.stride,))
        raw = np.array(mm_.reshape(k, self.stride)[:, self.frame_header:self.frame_header + self.fbytes])
        del mm_
        label = f"{os.path.basename(path)}#{first}-{last}"
        return label, unpack(raw, self.fmt, self.h, self.w)


# =============================================================================
# Wavelengths
# =============================================================================
def load_wavelengths(path: str | None, n_bands: int, wl_first: float, wl_last: float) -> np.ndarray:
    if path:
        text = open(path).read()
        m = re.search(r"wavelength\s*=\s*\{([^}]*)\}", text, re.IGNORECASE)   # ENVI .hdr
        body = m.group(1) if m else text
        vals = [float(v) for v in re.findall(r"[-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?", body)]
        if len(vals) != n_bands:
            sys.exit(f"{path}: found {len(vals)} wavelengths, frame has {n_bands} bands")
        return np.asarray(vals, dtype=np.float64)
    return np.linspace(wl_first, wl_last, n_bands)


# =============================================================================
# Edge analysis
# =============================================================================
def _erf(x: np.ndarray) -> np.ndarray:
    """Abramowitz & Stegun 7.1.26, |error| < 1.5e-7 (avoids a scipy dependency)."""
    s = np.sign(x)
    a = np.abs(x)
    t = 1.0 / (1.0 + 0.3275911 * a)
    y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t
               + 0.254829592) * t * np.exp(-a * a)
    return s * y


def fit_gaussian_esf(dx: np.ndarray, v: np.ndarray, s_max: float):
    """Fit v ~ a * Phi(dx / sigma) + b. Returns (sigma, a, b, rmse) or None."""
    if dx.size < 15:
        return None
    n = float(dx.size)
    sy, syy = float(v.sum()), float(v @ v)

    def sse(log_s):
        phi = 0.5 * (1.0 + _erf(dx / (math.sqrt(2.0) * math.exp(log_s))))
        sx, sxx, sxy = float(phi.sum()), float(phi @ phi), float(phi @ v)
        den = n * sxx - sx * sx
        if den <= 1e-12:
            return float("inf"), 0.0, 0.0
        a = (n * sxy - sx * sy) / den
        b = (sy - a * sx) / n
        r = syy - 2 * a * sxy - 2 * b * sy + a * a * sxx + 2 * a * b * sx + n * b * b
        return max(r, 0.0), a, b

    lo, hi = math.log(0.1), math.log(s_max)
    g = (math.sqrt(5.0) - 1.0) / 2.0
    c, d = hi - g * (hi - lo), lo + g * (hi - lo)
    fc, fd = sse(c)[0], sse(d)[0]
    for _ in range(40):
        if fc < fd:
            hi, d, fd = d, c, fc
            c = hi - g * (hi - lo)
            fc = sse(c)[0]
        else:
            lo, c, fc = c, d, fd
            d = lo + g * (hi - lo)
            fd = sse(d)[0]
    log_s = 0.5 * (lo + hi)
    r, a, b = sse(log_s)
    return math.exp(log_s), a, b, math.sqrt(r / n)


@dataclass
class Edge:
    x: float       # sub-pixel centre, spatial px
    pol: int       # +1 rising, -1 falling


def _edge_at(p: np.ndarray, x_guess: float, W: int, min_step: float):
    """Plateaus, step and 50 % crossing near x_guess. Returns (x, left, right) or None."""
    n = p.size
    xi = int(round(x_guess))
    if xi - W < 0 or xi + W >= n:
        return None
    left = p[xi - W: xi - W + 4]
    right = p[xi + W - 3: xi + W + 1]
    lm, rm = float(left.mean()), float(right.mean())
    step = rm - lm
    if abs(step) < min_step:
        return None
    if left.std() > 0.15 * abs(step) or right.std() > 0.15 * abs(step):
        return None
    mid = 0.5 * (lm + rm)
    best = None
    for i in range(max(xi - 3, 0), min(xi + 3, n - 1)):
        a, b = p[i] - mid, p[i + 1] - mid
        if a == b or a * b > 0:
            continue
        xc = i + a / (a - b)
        if best is None or abs(xc - x_guess) < abs(best - x_guess):
            best = xc
    if best is None:
        return None
    return best, lm, rm


def detect_edges(p: np.ndarray, x0: int, x1: int, W: int) -> list[Edge]:
    """Isolated edges with flat plateaus in p[x0:x1] (absolute coordinates)."""
    seg = p[x0:x1].astype(np.float64)
    if seg.size < 4 * W:
        return []
    rng = float(np.percentile(seg, 98) - np.percentile(seg, 2))
    if rng <= 0:
        return []
    ag = np.convolve(np.abs(np.gradient(seg)), [0.25, 0.5, 0.25], mode="same")
    thr = 0.08 * rng
    cand = [i for i in range(1, seg.size - 1)
            if ag[i] > thr and ag[i] >= ag[i - 1] and ag[i] > ag[i + 1]]
    merged: list[int] = []                       # merge maxima closer than 2 px
    for i in cand:
        if merged and i - merged[-1] < 2:
            if ag[i] > ag[merged[-1]]:
                merged[-1] = i
        else:
            merged.append(i)
    edges = []
    iso = W + 3                                  # neighbour must not reach the plateau window
    for k, i in enumerate(merged):
        dl = i - merged[k - 1] if k > 0 else 10 ** 9
        dr = merged[k + 1] - i if k + 1 < len(merged) else 10 ** 9
        if min(dl, dr) < iso:
            continue
        r = _edge_at(p, x0 + i, W, 0.2 * rng)
        if r is None:
            continue
        xc, lm, rm = r
        edges.append(Edge(xc, 1 if rm > lm else -1))
    return edges


@dataclass
class Params:
    bin_bands: int = 8
    half_window: int = 7
    roi: tuple[int, int] | None = None
    zones: int = 3
    wl_range: tuple[float, float] = (450.0, 900.0)


class Analyzer:
    def __init__(self, params: Params, wavelengths_fn):
        self.p = params
        self.wavelengths_fn = wavelengths_fn     # n_bands -> wavelengths

    def analyze(self, cube: np.ndarray) -> dict:
        """cube: (bands, spatial) float, averaged over frames."""
        P = self.p
        W = int(P.half_window)
        nb, ns = cube.shape
        wl = self.wavelengths_fn(nb)
        starts = np.arange(0, nb, max(1, P.bin_bands))
        counts = np.diff(np.append(starts, nb))
        prof = np.add.reduceat(cube, starts, axis=0) / counts[:, None]
        g_wl = np.add.reduceat(wl, starts) / counts
        in_rng = (g_wl >= P.wl_range[0]) & (g_wl <= P.wl_range[1])
        if not in_rng.any():
            in_rng[:] = True
        ref = prof[in_rng].mean(axis=0)
        x0, x1 = P.roi if P.roi else (0, ns)
        x0, x1 = max(0, int(x0)), min(ns, int(x1))
        edges = detect_edges(ref, x0, x1, W)
        zone_w = ns / max(1, P.zones)

        n_g = prof.shape[0]
        fwhm_g = np.full(n_g, np.nan)
        all_dx, all_v, all_z = [], [], []
        for gi in range(n_g):
            p = prof[gi]
            rng = float(np.percentile(p[x0:x1], 98) - np.percentile(p[x0:x1], 2)) if x1 > x0 else 0.0
            dxs, vs, zs = [], [], []
            for e in edges:
                r = _edge_at(p, e.x, W, 0.2 * rng)
                if r is None:
                    continue
                xc, lm, rm = r
                if (rm > lm) != (e.pol > 0):
                    continue
                idx = np.arange(int(math.floor(xc - W)), int(math.ceil(xc + W)) + 1)
                idx = idx[(idx >= 0) & (idx < ns)]
                dx = idx - xc
                keep = np.abs(dx) <= W
                dxs.append(dx[keep])
                vs.append((p[idx[keep]] - lm) / (rm - lm))
                zs.append(np.full(int(keep.sum()), min(int(xc // zone_w), P.zones - 1)))
            if not dxs:
                continue
            dx, v, z = np.concatenate(dxs), np.concatenate(vs), np.concatenate(zs)
            fit = fit_gaussian_esf(dx, v, s_max=W / 1.5)
            if fit:
                fwhm_g[gi] = FWHM_PER_SIGMA * fit[0]
            if in_rng[gi]:
                all_dx.append(dx)
                all_v.append(v)
                all_z.append(z)

        res = {"wl": g_wl, "fwhm_bands": fwhm_g, "edges": edges, "ref": ref, "roi": (x0, x1),
               "n_edges": len(edges), "fwhm": float("nan"), "sigma": float("nan"),
               "mtf50": float("nan"), "zones": [float("nan")] * P.zones,
               "esf_dx": np.empty(0), "esf_v": np.empty(0), "fit": None}
        if all_dx:
            dx, v, z = np.concatenate(all_dx), np.concatenate(all_v), np.concatenate(all_z)
            fit = fit_gaussian_esf(dx, v, s_max=W / 1.5)
            if fit:
                s, a, b, rmse = fit
                res.update(fwhm=FWHM_PER_SIGMA * s, sigma=s, mtf50=MTF50_SIGMA / s, fit=(s, a, b, rmse))
            for zi in range(P.zones):
                m = z == zi
                fz = fit_gaussian_esf(dx[m], v[m], s_max=W / 1.5) if m.sum() >= 15 else None
                res["zones"][zi] = FWHM_PER_SIGMA * fz[0] if fz else float("nan")
            sel = np.random.default_rng(0).permutation(dx.size)[:1500]
            res["esf_dx"], res["esf_v"] = dx[sel], v[sel]
        return res


# =============================================================================
# Session state
# =============================================================================
@dataclass
class FocusState:
    median_n: int = 3
    buf: collections.deque = field(default_factory=collections.deque)
    history: collections.deque = field(default_factory=lambda: collections.deque(maxlen=600))
    current: float | None = None
    best: float | None = None
    best_bands: np.ndarray | None = None
    result: dict | None = None
    sat_frac: float = 0.0
    label: str = "-"
    rate: float = 0.0
    _t_last: float = 0.0

    def push(self, res: dict, sat_frac: float, label: str) -> None:
        self.result, self.sat_frac, self.label = res, sat_frac, label
        now = time.time()
        if self._t_last:
            r = 1.0 / max(now - self._t_last, 1e-6)
            self.rate = 0.8 * self.rate + 0.2 * r if self.rate else r
        self._t_last = now
        f = res["fwhm"]
        if not math.isfinite(f):
            return
        self.buf.append(f)
        while len(self.buf) > max(1, self.median_n):
            self.buf.popleft()
        self.current = statistics.median(self.buf)
        self.best = self.current if self.best is None else min(self.best, self.current)
        fb = res["fwhm_bands"]
        if self.best_bands is None or self.best_bands.shape != fb.shape:
            self.best_bands = fb.copy()
        else:
            self.best_bands = np.fmin(self.best_bands, fb)
        self.history.append((now, self.current))

    @property
    def pct(self) -> float | None:
        if self.current is None or self.best is None:
            return None
        return 100.0 * self.best / self.current

    def reset(self) -> None:
        self.buf.clear()
        self.history.clear()
        self.current = self.best = self.best_bands = None


def zone_names(n: int) -> list[str]:
    return ["L", "C", "R"] if n == 3 else [f"Z{i + 1}" for i in range(n)]


# =============================================================================
# Shared processing step
# =============================================================================
class Pipeline:
    def __init__(self, args):
        self.args = args
        self.src = RawFrameSource(args.path, args.width, args.height, args.pixel_format,
                                  args.file_header, args.frame_header, args.frame_footer,
                                  replay=args.replay)
        self._wl_cache: dict[int, np.ndarray] = {}
        self.params = Params(bin_bands=args.bin_bands, half_window=args.half_window,
                             roi=tuple(args.roi) if args.roi else None, zones=args.zones,
                             wl_range=tuple(args.wl_range))
        self.analyzer = Analyzer(self.params, self._wavelengths)
        self.avg = args.avg

    def _wavelengths(self, nb: int) -> np.ndarray:
        if nb not in self._wl_cache:
            self._wl_cache[nb] = load_wavelengths(self.args.wavelengths, nb,
                                                  self.args.wl_first, self.args.wl_last)
        return self._wl_cache[nb]

    def step(self):
        """Returns (label, result, sat_frac) or None."""
        r = self.src.poll_block(self.avg)
        if r is None:
            return None
        label, frames = r
        if self.args.spectral_axis == "cols":
            frames = frames.transpose(0, 2, 1)
        if self.args.flip_spatial:
            frames = frames[:, :, ::-1]
        x0, x1 = self.params.roi if self.params.roi else (0, frames.shape[2])
        sat = float(np.mean(frames[-1][:, x0:x1] >= self.args.sat_level))
        cube = frames.astype(np.float32).mean(axis=0)
        return label, self.analyzer.analyze(cube), sat


# =============================================================================
# CLI
# =============================================================================
def run_cli(pl: Pipeline, st: FocusState) -> None:
    args = pl.args
    names = zone_names(args.zones)
    csv_f = None
    try:
        while True:
            out = pl.step()
            if out is None:
                if args.replay and pl.src.exhausted:
                    break
                time.sleep(args.poll)
                continue
            label, res, sat = out
            st.push(res, sat, label)
            if args.csv and csv_f is None:
                csv_f = open(args.csv, "w")
                csv_f.write("frames,n_edges,fwhm_px,median_px,best_px,pct_of_best,mtf50_cpp,sat_frac,"
                            + ",".join(f"fwhm_{n}" for n in names) + ","
                            + ",".join(f"fwhm_{w:.0f}nm" for w in res["wl"]) + "\n")
            f, pct = res["fwhm"], st.pct
            zs = "  ".join(f"{n} {z:4.2f}" for n, z in zip(names, res["zones"]))
            warn = f"  SAT {100 * sat:.1f}%!" if sat > 0.001 else ""
            line = (f"{label:>28s}  edges {res['n_edges']:3d}  FWHM {f:5.2f}px  "
                    f"best {st.best if st.best else float('nan'):5.2f}  "
                    f"{(pct or 0):5.1f}%  | {zs}{warn}")
            if args.replay:
                print(line)
            else:
                print("\r" + line, end="", flush=True)
            if csv_f:
                csv_f.write(f"{label},{res['n_edges']},{f:.4f},{st.current or float('nan'):.4f},"
                            f"{st.best or float('nan'):.4f},{pct or float('nan'):.2f},"
                            f"{res['mtf50']:.4f},{sat:.5f},"
                            + ",".join(f"{z:.4f}" for z in res["zones"]) + ","
                            + ",".join(f"{v:.4f}" for v in res["fwhm_bands"]) + "\n")
    except KeyboardInterrupt:
        pass
    finally:
        if csv_f:
            csv_f.close()
    print()
    if st.best is not None:
        print(f"session best FWHM (median-smoothed): {st.best:.3f} px")


# =============================================================================
# Web UI
# =============================================================================
def run_web(pl: Pipeline, st: FocusState) -> None:
    from nicegui import ui

    args = pl.args
    lock = threading.Lock()
    stop = threading.Event()

    def worker():
        while not stop.is_set():
            t0 = time.time()
            out = None
            try:
                with lock:
                    out = pl.step()
                    if out is not None:
                        label, res, sat = out
                        st.push(res, sat, label)
            except Exception as exc:          # keep the UI alive, show the problem in the console
                print(f"[worker] {type(exc).__name__}: {exc}", file=sys.stderr)
                time.sleep(1.0)
            dt = time.time() - t0
            time.sleep(max(args.poll if out is None else 0.0, args.min_interval - dt))

    threading.Thread(target=worker, daemon=True).start()
    names = zone_names(args.zones)

    @ui.page("/")
    def index():
        ui.label("FX10e focus monitor").classes("text-2xl font-bold")
        with ui.row().classes("items-end gap-10"):
            with ui.column().classes("gap-0"):
                ui.label("% of best").classes("text-sm text-gray-500")
                pct_lbl = ui.label("-").classes("text-7xl font-bold")
            with ui.column().classes("gap-0"):
                ui.label("LSF FWHM (px), median").classes("text-sm text-gray-500")
                cur_lbl = ui.label("-").classes("text-3xl font-mono")
            with ui.column().classes("gap-0"):
                ui.label("session best").classes("text-sm text-gray-500")
                best_lbl = ui.label("-").classes("text-3xl font-mono")
            with ui.column().classes("gap-0"):
                ui.label("per zone FWHM (px)").classes("text-sm text-gray-500")
                zone_lbl = ui.label("-").classes("text-2xl font-mono")
            with ui.column().classes("gap-0"):
                ui.label("edges / MTF50 / updates").classes("text-sm text-gray-500")
                info_lbl = ui.label("-").classes("text-base font-mono")
        warn_lbl = ui.label("").classes("text-lg font-bold text-red-600")

        P = pl.params
        with ui.row().classes("items-center gap-3"):
            ui.button("Reset best", on_click=lambda: _apply(None)).props("color=primary")
            ui.number("avg frames", value=pl.avg, min=1, max=500, step=1,
                      on_change=lambda e: _apply(("avg", e.value))).classes("w-28")
            ui.number("bands / group", value=P.bin_bands, min=1, max=64, step=1,
                      on_change=lambda e: _apply(("bin", e.value))).classes("w-28")
            ui.number("half window (px)", value=P.half_window, min=4, max=15, step=1,
                      on_change=lambda e: _apply(("W", e.value))).classes("w-32")
            ui.number("wl min (nm)", value=P.wl_range[0], step=10,
                      on_change=lambda e: _apply(("wl0", e.value))).classes("w-28")
            ui.number("wl max (nm)", value=P.wl_range[1], step=10,
                      on_change=lambda e: _apply(("wl1", e.value))).classes("w-28")
            roi0 = P.roi[0] if P.roi else 0
            roi1 = P.roi[1] if P.roi else args.width if args.spectral_axis == "rows" else args.height
            ui.number("ROI x0 (px)", value=roi0, min=0, step=10,
                      on_change=lambda e: _apply(("x0", e.value))).classes("w-28")
            ui.number("ROI x1 (px)", value=roi1, min=1, step=10,
                      on_change=lambda e: _apply(("x1", e.value))).classes("w-28")

        def _apply(change):
            with lock:
                if change is not None:
                    key, val = change
                    if val is None:
                        return
                    if key == "avg":
                        pl.avg = int(val)
                        return                       # averaging does not invalidate the best
                    if key == "bin":
                        P.bin_bands = int(val)
                    elif key == "W":
                        P.half_window = int(val)
                    elif key == "wl0":
                        P.wl_range = (float(val), P.wl_range[1])
                    elif key == "wl1":
                        P.wl_range = (P.wl_range[0], float(val))
                    elif key in ("x0", "x1"):
                        r0, r1 = P.roi if P.roi else (0, roi1)
                        P.roi = (int(val), r1) if key == "x0" else (r0, int(val))
                st.reset()

        def _chart(opts, h="h-60"):
            base = {"animation": False, "grid": {"left": 55, "right": 20, "top": 30, "bottom": 35},
                    "tooltip": {"trigger": "axis"}}
            base.update(opts)
            return ui.echart(base).classes(f"w-full {h}")

        with ui.row().classes("w-full no-wrap gap-4"):
            spec_chart = _chart({
                "title": {"text": "LSF FWHM vs wavelength", "left": 60, "textStyle": {"fontSize": 13}},
                "legend": {"top": 0, "right": 10},
                "xAxis": {"type": "value", "name": "nm", "scale": True},
                "yAxis": {"type": "value", "name": "px", "scale": True},
                "series": [{"type": "line", "name": "current", "showSymbol": False, "data": [],
                            "markArea": {"silent": True, "itemStyle": {"opacity": 0.08}, "data": []}},
                           {"type": "line", "name": "session best", "showSymbol": False, "data": [],
                            "lineStyle": {"type": "dashed"}}]})
            hist_chart = _chart({
                "title": {"text": "Overall FWHM (median) over time", "textStyle": {"fontSize": 13}},
                "xAxis": {"type": "value", "name": "s", "min": "dataMin"},
                "yAxis": {"type": "value", "name": "px", "scale": True},
                "series": [{"type": "line", "showSymbol": False, "data": []},
                           {"type": "line", "showSymbol": False, "data": [], "lineStyle": {"type": "dashed"}}]})
        with ui.row().classes("w-full no-wrap gap-4"):
            prof_chart = _chart({
                "title": {"text": "Spatial profile (bands in wl range) + detected edges",
                          "textStyle": {"fontSize": 13}},
                "xAxis": {"type": "value", "name": "px", "min": 0},
                "yAxis": {"type": "value", "name": "DN", "scale": True},
                "series": [{"type": "line", "showSymbol": False, "data": [], "lineStyle": {"width": 1},
                            "markArea": {"silent": True, "itemStyle": {"opacity": 0.06}, "data": []}},
                           {"type": "scatter", "symbolSize": 7, "data": []}]})
            esf_chart = _chart({
                "title": {"text": "Stacked edges (ESF) + Gaussian fit", "textStyle": {"fontSize": 13}},
                "tooltip": {"trigger": "item"},
                "xAxis": {"type": "value", "name": "px from edge"},
                "yAxis": {"type": "value", "min": -0.2, "max": 1.2},
                "series": [{"type": "scatter", "symbolSize": 3, "data": []},
                           {"type": "line", "showSymbol": False, "data": [], "lineStyle": {"width": 2}}]})

        def refresh():
            with lock:
                res = st.result
                pct, cur, best = st.pct, st.current, st.best
                best_bands = None if st.best_bands is None else st.best_bands.copy()
                sat, rate = st.sat_frac, st.rate
                hist = list(st.history)
                wl_rng = P.wl_range
            if res is None:
                return
            if pct is None:
                pct_lbl.text = "-"
            else:
                pct_lbl.text = f"{pct:5.1f}%"
                color = "text-green-600" if pct >= 95 else "text-amber-500" if pct >= 80 else "text-red-600"
                pct_lbl.classes(replace=f"text-7xl font-bold {color}")
            cur_lbl.text = "-" if cur is None else f"{cur:.2f}"
            best_lbl.text = "-" if best is None else f"{best:.2f}"
            zone_lbl.text = "  ".join(f"{n} {z:.2f}" if math.isfinite(z) else f"{n} -"
                                      for n, z in zip(names, res["zones"]))
            mtf = res["mtf50"]
            mtf_s = f"{mtf:.3f} c/px" if math.isfinite(mtf) else "-"
            info_lbl.text = f"{res['n_edges']} edges  {mtf_s}  {rate:4.1f}/s"
            warns = []
            if sat > 0.001:
                warns.append(f"Saturated: {100 * sat:.1f}% of ROI pixels >= {args.sat_level}, reduce exposure")
            if res["n_edges"] < 4:
                warns.append("Few edges found: check target placement, ROI, light or coarse focus")
            warn_lbl.text = " | ".join(warns)

            wl = res["wl"]
            fb = res["fwhm_bands"]
            spec_chart.options["series"][0]["data"] = [[round(float(w), 1), round(float(f), 3)]
                                                       for w, f in zip(wl, fb) if math.isfinite(f)]
            spec_chart.options["series"][0]["markArea"]["data"] = [[{"xAxis": wl_rng[0]}, {"xAxis": wl_rng[1]}]]
            spec_chart.options["series"][1]["data"] = (
                [] if best_bands is None or best_bands.shape != fb.shape else
                [[round(float(w), 1), round(float(f), 3)] for w, f in zip(wl, best_bands) if math.isfinite(f)])
            spec_chart.update()

            if hist:
                t0 = hist[0][0]
                hist_chart.options["series"][0]["data"] = [[round(t - t0, 2), round(v, 3)] for t, v in hist]
                hist_chart.options["series"][1]["data"] = [[0, best], [round(hist[-1][0] - t0, 2), best]]
            else:
                hist_chart.options["series"][0]["data"] = hist_chart.options["series"][1]["data"] = []
            hist_chart.update()

            ref = res["ref"]
            step = max(1, ref.size // 1024)
            prof_chart.options["series"][0]["data"] = [[i, round(float(ref[i]), 1)]
                                                       for i in range(0, ref.size, step)]
            x0, x1 = res["roi"]
            prof_chart.options["series"][0]["markArea"]["data"] = [[{"xAxis": x0}, {"xAxis": x1}]]
            prof_chart.options["series"][1]["data"] = [
                [round(e.x, 2), round(float(np.interp(e.x, np.arange(ref.size), ref)), 1)] for e in res["edges"]]
            prof_chart.options["xAxis"]["max"] = int(ref.size)
            prof_chart.update()

            esf_chart.options["series"][0]["data"] = [[round(float(a), 3), round(float(b), 3)]
                                                      for a, b in zip(res["esf_dx"], res["esf_v"])]
            if res["fit"]:
                s, a, b, _ = res["fit"]
                W = P.half_window
                xs = np.linspace(-W, W, 121)
                ys = a * 0.5 * (1 + _erf(xs / (math.sqrt(2) * s))) + b
                esf_chart.options["series"][1]["data"] = [[round(float(x), 3), round(float(y), 4)]
                                                          for x, y in zip(xs, ys)]
            esf_chart.update()

        ui.timer(args.ui_interval, refresh)

    ui.run(host=args.host, port=args.port, title="FX10e focus monitor", reload=False, show=False)


# =============================================================================
# Main
# =============================================================================
def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_argument_group("frame source")
    g.add_argument("--path", required=True, help="binary file or glob (newest file is followed)")
    g.add_argument("--width", type=int, required=True, help="frame width as delivered (e.g. 1024)")
    g.add_argument("--height", type=int, required=True, help="frame height as delivered (e.g. 224)")
    g.add_argument("--pixel-format", default="u16", choices=["u16", "mono12p", "mono12packed"])
    g.add_argument("--file-header", type=int, default=0, help="bytes before the first frame")
    g.add_argument("--frame-header", type=int, default=0, help="bytes before each frame's pixels")
    g.add_argument("--frame-footer", type=int, default=0, help="bytes after each frame's pixels")
    g.add_argument("--spectral-axis", default="rows", choices=["rows", "cols"])
    g.add_argument("--flip-spatial", action="store_true", help="mirror spatial axis so L/C/R match the field")
    g.add_argument("--wavelengths", help="file with one wavelength per band (txt/csv/ENVI .hdr)")
    g.add_argument("--wl-first", type=float, default=400.0, help="linear map: first band (nm)")
    g.add_argument("--wl-last", type=float, default=1000.0, help="linear map: last band (nm)")

    a = ap.add_argument_group("analysis")
    a.add_argument("--avg", type=int, default=10, help="frames averaged per update (default 10)")
    a.add_argument("--bin-bands", type=int, default=8, help="bands per group (default 8)")
    a.add_argument("--wl-range", type=float, nargs=2, default=[450.0, 900.0], metavar=("MIN", "MAX"),
                   help="wavelength range for the overall metric (default 450 900)")
    a.add_argument("--half-window", type=int, default=7, help="ESF half window, px (default 7)")
    a.add_argument("--roi", type=int, nargs=2, metavar=("X0", "X1"), help="spatial ROI, px (default all)")
    a.add_argument("--zones", type=int, default=3, help="field zones for per-zone FWHM (default 3 = L/C/R)")
    a.add_argument("--median", type=int, default=3, help="running median over N updates (default 3)")
    a.add_argument("--sat-level", type=int, default=4095, help="saturation level (default 4095, 12-bit)")

    m = ap.add_argument_group("mode")
    m.add_argument("--ui", choices=["cli", "web"], default="cli")
    m.add_argument("--replay", action="store_true", help="process the file block by block, then exit (CLI)")
    m.add_argument("--csv", help="write per-update results (incl. per-band FWHM) to CSV")
    m.add_argument("--host", default="0.0.0.0")
    m.add_argument("--port", type=int, default=8091)
    m.add_argument("--poll", type=float, default=0.05, help="poll interval when idle, s")
    m.add_argument("--min-interval", type=float, default=0.1, help="minimum time between updates, s")
    m.add_argument("--ui-interval", type=float, default=0.3, help="web UI refresh, s")
    return ap


def main() -> None:
    args = build_parser().parse_args()
    pl = Pipeline(args)
    st = FocusState(median_n=args.median)
    if args.ui == "web":
        run_web(pl, st)
    else:
        run_cli(pl, st)


if __name__ == "__main__":
    main()
