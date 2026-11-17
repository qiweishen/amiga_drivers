"""GOX ETTR on a ColorChecker exposure series: the brightest patch's peak channel.

The fit and suggestion arithmetic is ``app.decoder.exposure``. Only patches
without clipped photosites and below the near-saturation limit are fitted;
a clipped patch's mean is merely a lower bound of its signal.
"""

from __future__ import annotations

import numpy as np

from .. import exposure as ettr

TARGET_FRACTION = 0.80  # ETTR: brightest patch peak channel at 80 % of full scale
BRACKET_STEP_MS = 50.0
_CHANNELS = "RGB"


def bracket(center_ms: float) -> list[float]:
    """center ± 50 ms; when center <= 50 ms the low step is center / 2."""
    return ettr.bracket(center_ms, BRACKET_STEP_MS)


def brightest_patch(means: np.ndarray) -> int:
    """1-based index of the brightest patch (MCC24: #19 white) of one capture."""
    means = np.asarray(means, dtype=np.float64)
    if means.ndim != 2 or means.shape[1] != 3 or means.size == 0:
        raise ValueError("Patch means must be an (N, 3) array")
    return int(np.argmax(means.sum(axis=1))) + 1


def point_from_means(label: str, means: np.ndarray, status: list[str | None], patch: int,
                     exposure_time_us: float, gain: float, full_scale: int, *,
                     requested_exposure_ms: float | None = None, source: str = "") -> ettr.ExposurePoint:
    means = np.asarray(means, dtype=np.float64)
    if not 1 <= patch <= len(means) or len(status) != len(means):
        raise ValueError("Patch index or status does not match the measured chart")
    values = means[patch - 1] * full_scale
    channel = int(np.argmax(values))
    return ettr.ExposurePoint(label, float(exposure_time_us), float(gain), patch, float(values[channel]),
                              _CHANNELS[channel], status[patch - 1], requested_exposure_ms, source)
