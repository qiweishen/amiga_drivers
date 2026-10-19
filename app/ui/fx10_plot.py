"""Shared chart options for the FX10 snapshot workbench and white/dark references.

Statistics remain in recorded digital numbers (DN); no normalization, clipping,
or interpolation of the signal is performed. Captured wavelength coordinates
are preserved. Without wavelength metadata, band positions are mapped evenly
onto the user-requested 400–1000 nm display scale (one band is placed at 700).
That fallback is a display convention, not wavelength calibration, and must be
identified as uncalibrated by the caller. No capture metadata is modified.
"""

from __future__ import annotations

from collections.abc import Sequence


def axis_note(wavelengths_nm: Sequence[float]) -> str:
    """Describe the coordinate source for a persistent workbench annotation."""
    if len(wavelengths_nm):
        return "Wavelength coordinates recorded with this capture."
    return ("Uncalibrated linear display mapping: band positions span 400–1000 nm; "
            "this is not wavelength calibration.")


def workbench_options(
    spectrum_dn: dict[str, list[float]] | None = None,
    *,
    full_scale: int | None = None,
    bands: int = 0,
    wavelengths_nm: Sequence[float] = (),
) -> dict:
    """Build raw-DN curves and independent full-scale reference lines.

    ``full_scale`` is the maximum value of the captured PixelFormat (for example
    4095 for Mono12), not the uint16 storage-container maximum. Its 20/50/80%
    guides describe digital full scale, not a calibrated detector response.
    An omitted band count is inferred from the supplied statistics. Inconsistent
    dimensions are rejected instead of silently truncating a curve with ``zip``.
    """
    options = {
        "animation": False,
        "xAxis": {
            "type": "value", "name": "Wavelength (nm)",
            "nameLocation": "middle", "nameGap": 30,
            "min": 400, "max": 1000, "interval": 100,
        },
        "yAxis": {"type": "value", "name": "Raw signal (DN)", "min": 0},
        "grid": {"left": 64, "right": 24, "top": 56, "bottom": 52},
        "legend": {"top": 0, "data": []},
        "tooltip": {"trigger": "axis"},
        "series": [],
    }
    if not spectrum_dn or all(not values for values in spectrum_dn.values()):
        return options

    if bands < 0:
        raise ValueError("Band count must not be negative")
    count = bands or len(next(iter(spectrum_dn.values())))
    if count < 1 or any(len(values) != count for values in spectrum_dn.values()):
        raise ValueError("Spectral statistics do not match the band count")
    if full_scale is not None and full_scale <= 0:
        raise ValueError("Captured full scale must be positive")

    if len(wavelengths_nm):
        if len(wavelengths_nm) != count:
            raise ValueError("Wavelength coordinates do not match the band count")
        axis = list(wavelengths_nm)
    elif count == 1:
        axis = [700.0]
    else:
        axis = [400.0 + 600.0 * index / (count - 1) for index in range(count)]

    maximum = max(value for values in spectrum_dn.values() for value in values)
    options["yAxis"]["max"] = max(full_scale or 0, maximum)
    options["legend"]["data"] = list(spectrum_dn)
    options["series"] = [
        {
            "name": name, "type": "line", "showSymbol": count == 1, "symbolSize": 4,
            "animation": False,
            "data": [[coordinate, value] for coordinate, value in zip(axis, values)],
        }
        for name, values in spectrum_dn.items()
    ]

    if full_scale is not None:
        # This series is deliberately absent from legend.data, so hiding any
        # statistic (including the mean) leaves the reference lines visible.
        options["series"].append({
            "id": "fx10-full-scale-guides",
            "name": "Full-scale guides",
            "type": "line", "data": [], "showSymbol": False,
            "silent": True, "animation": False,
            "tooltip": {"show": False},
            "markLine": {
                "silent": True, "animation": False,
                "symbol": ["none", "none"],
                "lineStyle": {
                    "type": "dashed", "color": "#687681",
                    "opacity": 0.45, "width": 1,
                },
                "label": {
                    "show": True, "formatter": "{b}",
                    "position": "insideEndTop", "color": "#4e5b65",
                    "opacity": 1, "fontSize": 11,
                },
                "data": [
                    {"name": f"{percent}% full scale", "yAxis": full_scale * percent / 100.0}
                    for percent in (20, 50, 80)
                ],
            },
        })
    return options
