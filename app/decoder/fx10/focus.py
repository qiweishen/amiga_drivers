"""Focus on the FX10 edge target: LSF FWHM along the whole scan line (Camera Tools focus assist).

Port of resource/fx10e_focus_monitor.py without its spatial ROI: the FX10 images one
line, so every edge on it is measured. One SDK frame is that line (height = bands,
width = spatial samples); camera and target are still, so the service averages the
newest frames into one (bands, samples) cube. Then:

1. The bands are averaged in BAND_GROUPS equal groups.
2. Edges are found on the mean profile of the groups inside SCORED_NM. Only isolated
   edges with flat plateaus on both sides are kept, so a bar ladder or a V slit
   locator is skipped and only the edge array is measured.
3. In every group each edge is re-centred on its 50 % crossing and normalized to 0..1
   with its own plateaus. All edges are stacked into one oversampled edge spread
   function and a Gaussian ESF ``a * Phi(dx / sigma) + b`` is fitted; the line spread
   function FWHM is 2.3548 sigma, in spatial pixels.
4. The focus value is the same fit over all edges of the groups inside SCORED_NM; each
   third of the line (L / C / R) gets its own fit.

Lower FWHM is sharper. It is the system LSF including pixel sampling, so about
1.0-1.3 px is the floor; edges must be sharper than about HALF_WINDOW_PX, so coarse
focus by eye first. Without recorded wavelengths the bands are placed evenly on
400-1000 nm, the GUI's uncalibrated display mapping (ui/fx10_plot.py), and SCORED_NM
selects bands by that mapping, not by a calibration.

Screen images are the mean DN themselves (linear DN / full scale at 8 bit): no
stretch or tone curve. The RAW payload is only read.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from ..gox.preview import png_bytes  # generic lossless 8/16-bit PNG encoder

FWHM_PER_SIGMA = 2.0 * math.sqrt(2.0 * math.log(2.0))  # 2.3548
HALF_WINDOW_PX = 7  # ESF half window; the plateaus are its outer 4 px on each side
BAND_GROUPS = 28  # 8 bands per group at the factory 2x spectral binning (224 bands)
SCORED_NM = (450.0, 900.0)  # band groups of the focus value; the extremes are noisy
NOMINAL_NM = (400.0, 1000.0)  # uncalibrated display mapping, as ui/fx10_plot.py
ZONES = 3  # L / C / R thirds of the line
MIN_FIT_POINTS = 15
EDGE_GRADIENT = 0.08  # edge candidates: smoothed |gradient| above this fraction of the p2-p98 range
MIN_STEP = 0.2  # an edge step must reach this fraction of the p2-p98 range
FLAT_PLATEAU = 0.15  # plateau standard deviation at most this fraction of the step
LOUPE_SIZE = (128, 64)  # magnifier window: spatial pixels x bands

# erf by linear interpolation of the standard library's math.erf (numpy has no erf, and
# the GUI image has no scipy): step 0.0005, interpolation error below 4e-8.
_ERF_X = np.linspace(-6.0, 6.0, 24001)
_ERF_Y = np.array([math.erf(x) for x in _ERF_X])
_GOLDEN = (math.sqrt(5.0) - 1.0) / 2.0


def erf(x: np.ndarray) -> np.ndarray:
    return np.interp(x, _ERF_X, _ERF_Y)  # |x| > 6 clamps to +-1, which erf equals at double precision


@dataclass(frozen=True)
class Edge:
    x: float  # sub-pixel 50 % crossing, spatial px
    rising: bool


@dataclass(frozen=True)
class Window:
    """Frame pixels [x0, x1) x [y0, y1): x = spatial sample, y = band."""

    x0: int
    y0: int
    x1: int
    y1: int


@dataclass(frozen=True)
class Measurement:
    fwhm_px: float | None  # focus value: all edges of the groups inside SCORED_NM
    zones_px: tuple[float | None, ...]  # the same fit per third of the line (L, C, R)
    edges: tuple[Edge, ...]
    group_nm: tuple[float, ...]  # band-group centres
    group_fwhm_px: tuple[float | None, ...]
    calibrated: bool  # group_nm from recorded wavelengths, else the NOMINAL_NM mapping


@dataclass(frozen=True)
class FocusSample:
    seq: int  # record sequence of the newest averaged frame
    frames: int  # frames averaged
    samples: int
    bands: int
    measurement: Measurement
    clipped_pct: float  # averaged frames' pixels at or above full scale
    image_png: bytes  # mean frame, bands x samples, one image pixel per frame pixel
    loupe: Window
    loupe_png: bytes  # the loupe window of image_png


def fit_gaussian_esf(dx: np.ndarray, v: np.ndarray, sigma_max: float) -> float | None:
    """sigma of the least-squares fit v ~ a * Phi(dx / sigma) + b, or None with too few points.

    a and b have a closed form for each sigma; sigma is found by golden-section search on
    log(sigma) within [0.1, sigma_max].
    """
    if dx.size < MIN_FIT_POINTS:
        return None
    n, sy = float(dx.size), float(v.sum())

    def sse(log_sigma: float) -> float:
        phi = 0.5 * (1.0 + erf(dx / (math.sqrt(2.0) * math.exp(log_sigma))))
        sx = float(phi.sum())
        den = n * float(phi @ phi) - sx * sx
        if den <= 1e-12:
            return math.inf
        a = (n * float(phi @ v) - sx * sy) / den
        residual = v - a * phi - (sy - a * sx) / n
        return float(residual @ residual)

    lo, hi = math.log(0.1), math.log(sigma_max)
    c, d = hi - _GOLDEN * (hi - lo), lo + _GOLDEN * (hi - lo)
    fc, fd = sse(c), sse(d)
    for _ in range(40):
        if fc < fd:
            hi, d, fd = d, c, fc
            c = hi - _GOLDEN * (hi - lo)
            fc = sse(c)
        else:
            lo, c, fc = c, d, fd
            d = lo + _GOLDEN * (hi - lo)
            fd = sse(d)
    return math.exp(0.5 * (lo + hi))


def _edge_at(profile: np.ndarray, x_guess: float, min_step: float) -> tuple[float, float, float] | None:
    """(50 % crossing, left plateau, right plateau) of the edge near x_guess, or None."""
    half = HALF_WINDOW_PX
    xi = int(round(x_guess))
    if xi - half < 0 or xi + half >= profile.size:
        return None
    left, right = profile[xi - half:xi - half + 4], profile[xi + half - 3:xi + half + 1]
    left_mean, right_mean = float(left.mean()), float(right.mean())
    step = right_mean - left_mean
    if abs(step) < min_step or max(left.std(), right.std()) > FLAT_PLATEAU * abs(step):
        return None
    mid = 0.5 * (left_mean + right_mean)
    best = None
    for i in range(max(xi - 3, 0), min(xi + 3, profile.size - 1)):
        a, b = profile[i] - mid, profile[i + 1] - mid
        if a == b or a * b > 0:
            continue
        crossing = i + a / (a - b)
        if best is None or abs(crossing - x_guess) < abs(best - x_guess):
            best = crossing
    return None if best is None else (best, left_mean, right_mean)


def _range(profile: np.ndarray) -> float:
    return float(np.percentile(profile, 98) - np.percentile(profile, 2))


def detect_edges(profile: np.ndarray) -> tuple[Edge, ...]:
    """Isolated edges with flat plateaus on both sides, along the whole profile."""
    profile = np.asarray(profile, dtype=np.float64)
    span = _range(profile) if profile.size else 0.0
    if profile.size < 4 * HALF_WINDOW_PX or span <= 0:
        return ()
    gradient = np.convolve(np.abs(np.gradient(profile)), [0.25, 0.5, 0.25], mode="same")
    peaks: list[int] = []  # local maxima above the threshold; maxima closer than 2 px merge
    for i in range(1, profile.size - 1):
        if gradient[i] > EDGE_GRADIENT * span and gradient[i] >= gradient[i - 1] and gradient[i] > gradient[i + 1]:
            if peaks and i - peaks[-1] < 2:
                if gradient[i] > gradient[peaks[-1]]:
                    peaks[-1] = i
            else:
                peaks.append(i)
    isolation = HALF_WINDOW_PX + 3  # a neighbour must not reach the plateau window
    edges = []
    for k, i in enumerate(peaks):
        gap_left = i - peaks[k - 1] if k > 0 else isolation
        gap_right = peaks[k + 1] - i if k + 1 < len(peaks) else isolation
        if min(gap_left, gap_right) < isolation:
            continue
        found = _edge_at(profile, i, MIN_STEP * span)
        if found is not None:
            x, left, right = found
            edges.append(Edge(x, right > left))
    return tuple(edges)


def band_axis(bands: int, wavelengths_nm=()) -> tuple[np.ndarray, bool]:
    """Band centres in nm and whether they were recorded; else evenly on NOMINAL_NM."""
    if len(wavelengths_nm):
        if len(wavelengths_nm) != bands:
            raise ValueError("Wavelength axis does not match the band count")
        return np.asarray(wavelengths_nm, dtype=np.float64), True
    if bands == 1:
        return np.array([sum(NOMINAL_NM) / 2.0]), False
    return np.linspace(NOMINAL_NM[0], NOMINAL_NM[1], bands), False


def _stack_edges(profile: np.ndarray, edges: tuple[Edge, ...], zone_width: float):
    """(dx, normalized value, zone) of every edge of ``edges`` found again in ``profile``."""
    min_step = MIN_STEP * _range(profile)
    parts = []
    for edge in edges:
        found = _edge_at(profile, edge.x, min_step)
        if found is None:
            continue
        x, left, right = found
        if (right > left) != edge.rising:
            continue
        index = np.arange(math.ceil(x - HALF_WINDOW_PX), math.floor(x + HALF_WINDOW_PX) + 1)
        index = index[(index >= 0) & (index < profile.size)]
        zone = min(int(x // zone_width), ZONES - 1)
        parts.append((index - x, (profile[index] - left) / (right - left), np.full(index.size, zone)))
    if not parts:
        return np.empty(0), np.empty(0), np.empty(0, dtype=int)
    return tuple(np.concatenate(column) for column in zip(*parts))


def _fwhm(dx: np.ndarray, v: np.ndarray) -> float | None:
    sigma = fit_gaussian_esf(dx, v, HALF_WINDOW_PX / 1.5)
    return None if sigma is None else FWHM_PER_SIGMA * sigma


def measure(cube: np.ndarray, wavelengths_nm=()) -> Measurement:
    """LSF FWHM of a (bands, samples) cube over the whole line; see the module notes."""
    cube = np.asarray(cube, dtype=np.float64)
    if cube.ndim != 2 or cube.size == 0:
        raise ValueError("Focus needs a nonempty (bands, samples) cube")
    bands, samples = cube.shape
    nm, calibrated = band_axis(bands, wavelengths_nm)
    groups = np.array_split(np.arange(bands), min(BAND_GROUPS, bands))
    profiles = np.stack([cube[group].mean(axis=0) for group in groups])
    group_nm = np.array([nm[group].mean() for group in groups])
    scored = (group_nm >= SCORED_NM[0]) & (group_nm <= SCORED_NM[1])
    if not scored.any():
        scored[:] = True
    edges = detect_edges(profiles[scored].mean(axis=0))
    zone_width = samples / ZONES
    group_fwhm, stacked = [], []
    for profile, in_range in zip(profiles, scored):
        dx, v, zone = _stack_edges(profile, edges, zone_width)
        group_fwhm.append(_fwhm(dx, v))
        if in_range and dx.size:
            stacked.append((dx, v, zone))
    if stacked:
        dx, v, zone = (np.concatenate(column) for column in zip(*stacked))
    else:
        dx, v, zone = np.empty(0), np.empty(0), np.empty(0, dtype=int)
    return Measurement(
        fwhm_px=_fwhm(dx, v), zones_px=tuple(_fwhm(dx[zone == i], v[zone == i]) for i in range(ZONES)),
        edges=edges, group_nm=tuple(float(value) for value in group_nm), group_fwhm_px=tuple(group_fwhm),
        calibrated=calibrated,
    )


def loupe_window(samples: int, bands: int, center: tuple[float, float] | None) -> Window:
    """LOUPE_SIZE around ``center`` (x = sample, y = band; the frame centre when None), moved inside."""
    width, height = min(LOUPE_SIZE[0], samples), min(LOUPE_SIZE[1], bands)
    cx, cy = (samples / 2.0, bands / 2.0) if center is None else (float(center[0]), float(center[1]))
    if not (math.isfinite(cx) and math.isfinite(cy)):
        raise ValueError("Magnifier centre must be finite")
    x0 = min(max(math.floor(cx - width / 2.0), 0), samples - width)
    y0 = min(max(math.floor(cy - height / 2.0), 0), bands - height)
    return Window(x0, y0, x0 + width, y0 + height)


def display8(cube: np.ndarray, full_scale: int) -> np.ndarray:
    """Linear DN / full scale, quantized to 8 bits for the screen."""
    return np.rint(np.clip(cube / float(full_scale), 0.0, 1.0) * 255.0).astype(np.uint8)


def analyze(frames: np.ndarray, capture, seq: int, center: tuple[float, float] | None) -> FocusSample:
    """Average a (frames, bands, samples) native-DN stack, measure it and render the screen images.

    ``capture`` is the stream's decoder.fx10.Capture (full scale and wavelengths).
    """
    if frames.ndim != 3 or 0 in frames.shape:
        raise ValueError("Focus needs a nonempty (frames, bands, samples) stack")
    count, bands, samples = frames.shape
    cube = frames.mean(axis=0, dtype=np.float64)
    image = display8(cube, capture.full_scale)
    window = loupe_window(samples, bands, center)
    return FocusSample(
        seq=seq, frames=count, samples=samples, bands=bands,
        measurement=measure(cube, capture.wavelengths_nm),
        clipped_pct=float(np.count_nonzero(frames >= capture.full_scale)) / frames.size * 100.0,
        image_png=png_bytes(image), loupe=window,
        loupe_png=png_bytes(np.ascontiguousarray(image[window.y0:window.y1, window.x0:window.x1])),
    )
