"""ETTR exposure-bracket results shared by the GoX colour reference and the FX10 white reference."""

from __future__ import annotations

from collections.abc import Callable

from nicegui import ui

from ..decoder import exposure

STATE_LABELS = {None: "ok", "clipped": "saturated", "near_saturation": "near saturation"}


def check_text(check, full_scale: int, measured: str) -> str:
    """One verification against the series target; ``measured`` names the value ("patch #3 peak")."""
    if check is None:
        return ""
    point = check.point
    value = f"{measured} {point.peak_dn:.0f} DN ({point.peak_dn / full_scale:.1%} FS)"
    if check.deviation is None:
        return f"{value}, saturated. {check.message}"
    return (f"{value} vs target {check.target_dn:.0f} DN: {check.deviation:+.1%} "
            f"({'within' if check.within else 'outside'} ±{exposure.TOLERANCE:.0%}). {check.message}")


def fit_lines(series, first: str, unchanged: str) -> list[str]:
    """The fit, its target, ceiling notes, suggestion and warnings, one line each."""
    fit = series.fit
    lines = [first,
             f"Target {fit.target_dn:.0f} DN ({fit.target_fraction:.0%} FS) → {fit.suggested_exposure_ms:g} ms "
             f"({unchanged})"]
    if series.limit_notes:
        lines.append("Exposure ceiling: " + "; ".join(series.limit_notes))
    if series.initial_suggestion is not None:
        initial = series.initial_suggestion
        lines.append(f"Suggested: {initial.exposure_ms:g} ms" + (f" · {initial.note}" if initial.note else ""))
    return lines + [f"⚠ {warning}" for warning in fit.warnings]


class SeriesPanel:
    """Status line, bracket table, fit text and the Apply row of one exposure series."""

    def __init__(self, columns: list[dict], apply: Callable) -> None:
        self.status = ui.label("").classes("text-sm break-all")
        self.table = ui.table(columns=columns, rows=[], row_key="key").props("dense flat").classes("w-full")
        self.table.set_visibility(False)
        self.fit = ui.label("").classes("section-copy whitespace-pre-line")
        with ui.row().classes("console-toolbar w-full items-center") as self.apply_row:
            self.apply_button = ui.button("Apply suggested exposure to config", icon="save", on_click=apply)
            self.apply_note = ui.label("").classes("text-sm muted break-all")
        self.apply_row.set_visibility(False)
        self._shown = False  # never a series, so the first refresh draws

    def redraw(self) -> None:
        self._shown = False

    def refresh(self, series, running: bool, status: str, rows: Callable, text: Callable) -> None:
        """``rows(series)`` and ``text(series)`` are only called when the series changed."""
        if running:
            self.status.set_text(status)
        if series is self._shown:
            return
        self._shown = series
        self.table.set_visibility(series is not None and bool(series.points))
        self.table.rows = rows(series) if series is not None else []
        self.table.update()
        self.fit.set_text(text(series) if series is not None else "")
        if series is not None and not running:
            self.status.set_text(f"Exposure series: {series.directory}" if series.directory else "")
        suggestion = series.suggestion if series is not None else None
        self.apply_row.set_visibility(suggestion is not None)
        if suggestion is not None:
            self.apply_button.set_text(f"Apply exposure {suggestion.exposure_ms:g} ms to config")
