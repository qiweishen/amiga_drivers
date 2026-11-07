"""Per-patch CIEDE2000 of a chart fit: linear sRGB (D65) -> CIE L*a*b* (D65, 2 deg) -> ΔE00.

Matches OpenCV ccm's DISTANCE_CIE2000 on COLOR_SPACE_SRGB: the root mean
square of ``ciede2000`` over the fitted patches equals ``getLoss()``.
Formula: Sharma, Wu & Dalal (2005), "The CIEDE2000 color-difference formula".
"""

from __future__ import annotations

import numpy as np

_SRGB_TO_XYZ = np.array([[0.4124564, 0.3575761, 0.1804375],
                         [0.2126729, 0.7151522, 0.0721750],
                         [0.0193339, 0.1191920, 0.9503041]])
_WHITE_D65 = np.array([0.95047, 1.0, 1.08883])
_EPSILON = (6.0 / 29.0) ** 3


def lab_from_linear_srgb(rgb: np.ndarray) -> np.ndarray:
    """(N, 3) unclipped linear sRGB -> (N, 3) L*a*b*."""
    xyz = np.asarray(rgb, dtype=np.float64) @ _SRGB_TO_XYZ.T / _WHITE_D65
    f = np.where(xyz > _EPSILON, np.cbrt(xyz), xyz / (3.0 * (6.0 / 29.0) ** 2) + 4.0 / 29.0)
    return np.stack([116.0 * f[:, 1] - 16.0, 500.0 * (f[:, 0] - f[:, 1]), 200.0 * (f[:, 1] - f[:, 2])], axis=1)


def ciede2000(lab1: np.ndarray, lab2: np.ndarray) -> np.ndarray:
    """Row-wise ΔE00 between two (N, 3) L*a*b* arrays (kL = kC = kH = 1)."""
    L1, a1, b1 = np.asarray(lab1, dtype=np.float64).T
    L2, a2, b2 = np.asarray(lab2, dtype=np.float64).T
    c_mean = (np.hypot(a1, b1) + np.hypot(a2, b2)) / 2.0
    g = 0.5 * (1.0 - np.sqrt(c_mean ** 7 / (c_mean ** 7 + 25.0 ** 7)))
    a1p, a2p = (1.0 + g) * a1, (1.0 + g) * a2
    c1p, c2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360.0
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360.0
    chroma_zero = c1p * c2p == 0

    dl, dc = L2 - L1, c2p - c1p
    dh = h2p - h1p
    dh = np.where(dh > 180.0, dh - 360.0, np.where(dh < -180.0, dh + 360.0, dh))
    dh = np.where(chroma_zero, 0.0, dh)
    dh_big = 2.0 * np.sqrt(c1p * c2p) * np.sin(np.radians(dh / 2.0))

    l_mean, cp_mean = (L1 + L2) / 2.0, (c1p + c2p) / 2.0
    h_sum = h1p + h2p
    hp_mean = np.where(chroma_zero, h_sum,
                       np.where(np.abs(h1p - h2p) <= 180.0, h_sum / 2.0,
                                np.where(h_sum < 360.0, (h_sum + 360.0) / 2.0, (h_sum - 360.0) / 2.0)))
    t = (1.0 - 0.17 * np.cos(np.radians(hp_mean - 30.0)) + 0.24 * np.cos(np.radians(2.0 * hp_mean))
         + 0.32 * np.cos(np.radians(3.0 * hp_mean + 6.0)) - 0.20 * np.cos(np.radians(4.0 * hp_mean - 63.0)))
    d_theta = 30.0 * np.exp(-(((hp_mean - 275.0) / 25.0) ** 2))
    r_c = 2.0 * np.sqrt(cp_mean ** 7 / (cp_mean ** 7 + 25.0 ** 7))
    s_l = 1.0 + 0.015 * (l_mean - 50.0) ** 2 / np.sqrt(20.0 + (l_mean - 50.0) ** 2)
    s_c = 1.0 + 0.045 * cp_mean
    s_h = 1.0 + 0.015 * cp_mean * t
    r_t = -np.sin(np.radians(2.0 * d_theta)) * r_c
    return np.sqrt((dl / s_l) ** 2 + (dc / s_c) ** 2 + (dh_big / s_h) ** 2
                   + r_t * (dc / s_c) * (dh_big / s_h))
