"""Camera Tools · FX10 focus assist panel; streaming and measuring live in services.fx10_focus.

The frame is shown like eBUS Player: x = spatial sample, y = band, one image pixel
per frame pixel, so a bar target appears as vertical stripes. Ticks mark the edges
that are measured. Clicking the frame moves the magnifier and clicking the magnifier
re-centres it; neither changes the measurement, which always uses the whole line.
"""

from __future__ import annotations

from collections.abc import Callable

from nicegui import ui

from ..constants import UI_TICK_S
from ..decoder.fx10 import focus
from ..services import camera_operations, fx10_focus
from ..state import STATE
from . import fx10_plot
from .components import heading, hint, info, metric, preview_image

_GOOD_PCT, _NEAR_PCT = 95.0, 80.0
_CLIPPED_WARN_PCT = 0.5
_MIN_EDGES = 4
_MARK_COLOR = "#f39c12"


def _history_options() -> dict:
    return {
        "animation": False,
        "grid": {"left": 56, "right": 16, "top": 28, "bottom": 28},
        "tooltip": {"trigger": "axis"},
        "legend": {"top": 0, "right": 8, "data": ["FWHM", "Session best"]},
        "xAxis": {"type": "value", "name": "s", "min": "dataMin", "max": "dataMax"},
        "yAxis": {"type": "value", "name": "px", "scale": True},
        "series": [
            {"type": "line", "name": "FWHM", "showSymbol": False, "data": []},
            {"type": "line", "name": "Session best", "showSymbol": False, "data": [],
             "lineStyle": {"type": "dashed"}},
        ],
    }


def _band_options() -> dict:
    low, high = focus.SCORED_NM
    return {
        "animation": False,
        "grid": {"left": 56, "right": 16, "top": 28, "bottom": 28},
        "tooltip": {"trigger": "axis"},
        "legend": {"top": 0, "right": 8, "data": ["FWHM", "Session best"]},
        "xAxis": {"type": "value", "name": "nm", "min": "dataMin", "max": "dataMax"},
        "yAxis": {"type": "value", "name": "px", "scale": True},
        "series": [
            {"type": "line", "name": "FWHM", "showSymbol": False, "data": [],
             "markArea": {"silent": True, "itemStyle": {"opacity": 0.08},
                          "data": [[{"xAxis": low}, {"xAxis": high}]]}},
            {"type": "line", "name": "Session best", "showSymbol": False, "data": [],
             "lineStyle": {"type": "dashed"}},
        ],
    }


def _frame_svg(view: fx10_focus.FocusView) -> str:
    """Magnifier box and edge ticks in frame pixels (pixel i spans [i, i + 1))."""
    if not view.image_url or view.loupe is None or view.frame_size is None:
        return ""
    box = view.loupe
    bands = view.frame_size[1]
    tick = max(3.0, 0.1 * bands)
    parts = [f'<rect x="{box.x0}" y="{box.y0}" width="{box.x1 - box.x0}" height="{box.y1 - box.y0}" '
             f'fill="none" stroke="{_MARK_COLOR}" stroke-width="2" vector-effect="non-scaling-stroke" />']
    for edge in view.measurement.edges if view.measurement else ():
        x = edge.x + 0.5
        parts += [f'<line x1="{x:.2f}" y1="{y0:.1f}" x2="{x:.2f}" y2="{y1:.1f}" stroke="{_MARK_COLOR}" '
                  'stroke-width="2" vector-effect="non-scaling-stroke" />'
                  for y0, y1 in ((0.0, tick), (bands - tick, float(bands)))]
    return "".join(parts)


def _px(value: float | None) -> str:
    return "—" if value is None else f"{value:.2f}"


def _status_text(view: fx10_focus.FocusView) -> str:
    request = view.request
    if view.phase == "starting" and request:
        return f"Connecting to {request.target_ip} …"
    if view.phase == "live" and request:
        return (f"Live · {request.target_ip} · {request.exposure_ms:g} ms · {request.frame_rate_hz:g} Hz · "
                f"binning {request.spatial_binning}×{request.spectral_binning}")
    if view.phase == "stopping":
        return "Stopping …"
    return "Idle" + (f" · {view.message}" if view.message else "")


def focus_panel(request: Callable[[bool], fx10_focus.FocusRequest], target_ip: Callable[[], str],
                show_raw: Callable[[str], None]) -> None:
    """``request(remember)`` builds the stream request from the FX10 workbench values (ValueError
    when they are invalid); remember=True also stores them like Take snapshot does."""
    low, high = focus.SCORED_NM
    with ui.column().classes("console-panel w-full min-w-0"):
        with heading("FX10 · Focus assist", icon="center_focus_strong"):
            status = ui.label("").classes("text-xs mono muted break-all")
        hint(f"Aim for ≥ {_GOOD_PCT:g} % of the session best.",
             "Live stream with the workbench exposure, binning and frame rate. The newest frames "
             f"(about {fx10_focus.AVERAGE_S:g} s, at most {fx10_focus.MAX_AVERAGE_FRAMES}) are averaged and the edge "
             "target is measured along the whole line: the isolated edges are stacked into one edge profile and "
             "a Gaussian fit gives the line spread FWHM in spatial pixels. Lower is sharper; about 1.0–1.3 px is "
             f"the floor. The focus value uses the bands of {low:g}–{high:g} nm; L · C · R are the thirds of the "
             "line. Nothing is saved; the stream holds the camera until stopped.\n"
             "Set the exposure first (no clipping) and focus coarsely by eye until edges are ticked. Turn the "
             f"ring slowly through the best, come back to ≥ {_GOOD_PCT:g} % and lock it. Reset the best after "
             "changing height, aperture or light.")
        with ui.row().classes("console-toolbar"):
            start_btn = ui.button("Start live focus", icon="center_focus_strong", on_click=lambda: _start())
            stop_btn = ui.button("Stop", icon="stop", on_click=fx10_focus.stop).props("outline")
            reset_btn = ui.button("Reset best", icon="restart_alt", on_click=fx10_focus.reset_best).props("outline")
            busy = ui.spinner(size="sm")
        with ui.element("div").classes("overview-metrics"):
            pct_label = metric("Of session best", "focus-percent")
            fwhm_label = metric(f"LSF FWHM px · median of {fx10_focus.MEDIAN_UPDATES}")
            best_label = metric("Session best px")
            zones_label = metric("FWHM px · L · C · R")
        detail = ui.label("").classes("text-xs mono muted")
        warn = ui.label("").classes("text-sm text-amber-700")
        with ui.element("div").classes("workspace-grid"):
            with ui.column().classes("w-full min-w-0 gap-1"):
                ui.label("Frame · spatial × bands · click to move the magnifier").classes("text-xs muted")
                frame = preview_image("center_focus_strong", "No live frame", "cursor-crosshair",
                                      on_mouse=lambda event: _frame_click(event), events=["click"])
            with ui.column().classes("w-full min-w-0 gap-1"):
                with ui.row().classes("hint-row"):
                    ui.label("Magnifier · click to centre").classes("text-xs muted")
                    width, height = focus.LOUPE_SIZE
                    info(f"{width} × {height} frame pixels (spatial × bands), one image pixel each, enlarged "
                         "without smoothing. Display only: the FWHM always uses the whole line.")
                loupe = preview_image("zoom_in", "No magnified frame", "focus-loupe cursor-crosshair",
                                      on_mouse=lambda event: _loupe_click(event), events=["click"])
        with ui.element("div").classes("workspace-grid"):
            with ui.column().classes("plot-surface w-full min-w-0"):
                history_chart = ui.echart(_history_options()).classes("w-full h-48")
            with ui.column().classes("plot-surface w-full min-w-0"):
                band_chart = ui.echart(_band_options()).classes("w-full h-48")
                axis_note = ui.label("").classes("text-xs muted")

    seen: dict = {"version": None, "phase": fx10_focus.view().phase}

    def _start() -> None:
        try:
            wanted = request(True)
        except (TypeError, ValueError) as error:
            ui.notify(str(error), type="warning")
            return
        try:
            fx10_focus.start(wanted)
        except Exception as error:
            ui.notify(f"Live focus did not start: {error}", type="negative", multi_line=True)
        refresh()

    def _frame_click(event) -> None:
        if fx10_focus.view().image_url:
            fx10_focus.set_loupe_center(event.image_x, event.image_y)

    def _loupe_click(event) -> None:
        view = fx10_focus.view()
        if view.loupe_url and view.loupe is not None:  # one magnifier pixel = one frame pixel
            fx10_focus.set_loupe_center(view.loupe.x0 + event.image_x, view.loupe.y0 + event.image_y)

    def _warnings(view: fx10_focus.FocusView) -> str:
        parts = []
        if view.clipped_pct is not None and view.clipped_pct > _CLIPPED_WARN_PCT:
            parts.append(f"{view.clipped_pct:.1f} % of pixels clipped: lower the exposure.")
        if view.measurement is not None and len(view.measurement.edges) < _MIN_EDGES:
            parts.append(f"Few edges ({len(view.measurement.edges)}): check the target, light or coarse focus.")
        if view.phase in ("starting", "live") and view.request is not None:
            try:
                wanted = request(False)
            except (TypeError, ValueError):
                wanted = None
            if wanted != view.request:
                parts.append("Workbench changed: stop and start to apply.")
        if view.message_is_error and view.phase != "idle":
            parts.append(view.message)
        return " ".join(parts)

    def refresh() -> None:
        fx10_focus.touch()
        view = fx10_focus.view()
        start_btn.set_enabled(view.phase == "idle" and camera_operations.guard_reason("fx10") is None
                              and not STATE.snapshot_busy and bool(target_ip()))
        stop_btn.set_enabled(view.phase in ("starting", "live"))
        reset_btn.set_enabled(view.best is not None)
        busy.set_visibility(view.phase in ("starting", "stopping"))
        status.set_text(_status_text(view))
        warn.set_text(_warnings(view))
        if view.phase == "idle" and seen["phase"] != "idle":  # a stream just ended
            if view.raw_output:
                show_raw(view.raw_output)
            if view.message:
                ui.notify(view.message, type="negative" if view.message_is_error else "info", multi_line=True)
        seen["phase"] = view.phase
        if view.version == seen["version"]:
            return
        seen["version"] = view.version

        measurement = view.measurement
        pct = view.pct
        if pct is None:
            pct_label.set_text("no edges" if measurement is not None and measurement.fwhm_px is None else "—")
            pct_label.classes(replace="focus-percent")
        else:
            tone = "focus-good" if pct >= _GOOD_PCT else "focus-near" if pct >= _NEAR_PCT else "focus-far"
            pct_label.set_text(f"{pct:5.1f} %")
            pct_label.classes(replace=f"focus-percent {tone}")
        fwhm_label.set_text(_px(view.current))
        best_label.set_text(_px(view.best))
        zones_label.set_text(" · ".join(_px(zone) for zone in measurement.zones_px) if measurement else "—")
        detail.set_text("" if measurement is None else
                        f"{len(measurement.edges)} edges · {view.frames} frames averaged · {view.rate:.1f} updates/s")

        series = history_chart.options["series"]
        series[0]["data"] = [list(point) for point in view.history]
        series[1]["data"] = ([[view.history[0][0], view.best], [view.history[-1][0], view.best]]
                             if view.history and view.best is not None else [])
        history_chart.update()

        series = band_chart.options["series"]
        if measurement is None:
            series[0]["data"] = series[1]["data"] = []
            axis_note.set_text("")
        else:
            nm = measurement.group_nm
            series[0]["data"] = [[round(x, 1), round(y, 3)] for x, y in zip(nm, measurement.group_fwhm_px)
                                 if y is not None]
            series[1]["data"] = ([[round(x, 1), round(y, 3)] for x, y in zip(nm, view.best_groups) if y is not None]
                                 if len(view.best_groups) == len(nm) else [])
            axis_note.set_text(fx10_plot.axis_note(nm if measurement.calibrated else ()))
        band_chart.update()

        frame.set_source(view.image_url)
        loupe.set_source(view.loupe_url)
        frame.set_content(_frame_svg(view))

    # No synchronous first refresh: the caller's callbacks may be defined after this panel is
    # built; the timer first fires once the page is connected.
    ui.timer(UI_TICK_S, refresh)
