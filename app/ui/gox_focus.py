"""Camera Tools · GoX focus assist panel; streaming and scoring live in services.gox_focus.

The operator places the ROI by clicking the target in the overview and centres
it by clicking in the magnifier. The magnifier shows the scored Gr photosites
one image pixel per photosite, enlarged by the browser without smoothing, so the
ROI size also sets the magnification.
"""

from __future__ import annotations

from collections.abc import Callable

from nicegui import app, ui

from ..constants import UI_TICK_S
from ..decoder.gox import focus
from ..services import camera_operations, gox_focus, timing
from ..state import STATE
from .components import heading, hint, info, metric, preview_image

_GOOD_PCT, _NEAR_PCT = 95.0, 80.0
_CLIPPED_WARN_PCT = 0.5
_ROI_COLOR = "#f39c12"


def _chart_options() -> dict:
    return {
        "animation": False,
        "grid": {"left": 64, "right": 16, "top": 24, "bottom": 28},
        "tooltip": {"trigger": "axis"},
        "legend": {"top": 0, "right": 8, "data": ["Score", "Session peak"]},
        "xAxis": {"type": "value", "name": "s", "min": "dataMin", "max": "dataMax"},
        "yAxis": {"type": "value", "scale": True},
        "series": [
            {"type": "line", "name": "Score", "showSymbol": False, "data": []},
            {"type": "line", "name": "Session peak", "showSymbol": False, "data": [],
             "lineStyle": {"type": "dashed"}},
        ],
    }


def _roi_svg(view: gox_focus.FocusView) -> str:
    """The ROI box in overview image pixels."""
    if view.roi is None or not view.overview_url:
        return ""
    sx, sy = view.overview_scale
    roi = view.roi
    return (f'<rect x="{roi.x0 / sx:.1f}" y="{roi.y0 / sy:.1f}" width="{(roi.x1 - roi.x0) / sx:.1f}" '
            f'height="{(roi.y1 - roi.y0) / sy:.1f}" fill="none" stroke="{_ROI_COLOR}" stroke-width="2" '
            'vector-effect="non-scaling-stroke" />')


def _loupe_svg(view: gox_focus.FocusView) -> str:
    """A crosshair with an open centre at the ROI centre, in magnifier pixels (Gr photosites)."""
    if view.roi is None or not view.loupe_url:
        return ""
    cx, cy = view.roi.center
    ox, oy = view.loupe_origin
    x, y = (cx - ox) / 2.0, (cy - oy) / 2.0
    gap, arm = 3.0, 12.0
    segments = ((x - arm, y, x - gap, y), (x + gap, y, x + arm, y),
                (x, y - arm, x, y - gap), (x, y + gap, x, y + arm))
    return "".join(f'<line x1="{a:.1f}" y1="{b:.1f}" x2="{c:.1f}" y2="{d:.1f}" stroke="{_ROI_COLOR}" '
                   'stroke-width="1.5" vector-effect="non-scaling-stroke" />' for a, b, c, d in segments)


def _status_text(view: gox_focus.FocusView) -> str:
    request = view.request
    if view.phase == "starting" and request:
        return f"Connecting to {request.target_ip} …"
    if view.phase == "live" and request:
        return (f"Live · {request.target_ip} · {request.exposure_ms:g} ms · {request.frame_rate_hz:g} Hz · "
                f"gain x{request.gain:g}")
    if view.phase == "stopping":
        return "Stopping …"
    return "Idle" + (f" · {view.message}" if view.message else "")


def focus_panel(request: Callable[[bool], gox_focus.FocusRequest], target_ip: Callable[[], str],
                show_raw: Callable[[str], None]) -> None:
    """``request(remember)`` builds the stream request from the GoX workbench values (ValueError
    when they are invalid); remember=True also stores them like Take snapshot does."""
    if gox_focus.view().phase == "idle":
        gox_focus.set_roi_half(app.storage.general.get("gox_focus_roi_half", gox_focus.DEFAULT_HALF_PX))

    with ui.column().classes("console-panel w-full min-w-0"):
        with heading("GoX · Focus assist", icon="center_focus_strong"):
            status = ui.label("").classes("text-xs mono muted break-all")
        hint(f"Aim for ≥ {_GOOD_PCT:g} % of the session peak.",
             f"Live 8-bit {focus.FOCUS_PIXEL_FORMAT} stream with the workbench exposure and gain, at the highest "
             f"rate that exposure allows (≤ {timing.GOX_MAX_FOCUS_FRAME_RATE_HZ:g} Hz). Focus is scored on the Gr "
             "photosites in the ROI. Nothing is saved; the stream holds the camera until stopped.\n"
             "Set the exposure first (no clipping in the ROI). Turn the ring slowly through the peak, come back "
             f"to ≥ {_GOOD_PCT:g} % and lock it. Reset the peak after changing height, aperture or light.")
        with ui.row().classes("console-toolbar"):
            start_btn = ui.button("Start live focus", icon="center_focus_strong", on_click=lambda: _start())
            stop_btn = ui.button("Stop", icon="stop", on_click=gox_focus.stop).props("outline")
            reset_btn = ui.button("Reset peak", icon="restart_alt", on_click=gox_focus.reset_peak).props("outline")
            ui.number("ROI half-size", value=gox_focus.view().roi_half, min=focus.MIN_HALF_PX,
                      max=focus.MAX_HALF_PX, step=16, suffix="px",
                      on_change=lambda event: _set_half(event.value)).props("dense outlined").classes("w-40")
            busy = ui.spinner(size="sm")
        with ui.element("div").classes("overview-metrics"):
            pct_label = metric("Of session peak", "focus-percent")
            score_label = metric(f"Score · median of {gox_focus.MEDIAN_FRAMES} frames")
            peak_label = metric("Session peak")
            frame_label = metric("Frame · analysed rate")
        warn = ui.label("").classes("text-sm text-amber-700")
        with ui.element("div").classes("workspace-grid"):
            with ui.column().classes("w-full min-w-0 gap-1"):
                ui.label("Overview · click to move the ROI").classes("text-xs muted")
                overview = preview_image("center_focus_strong", "No live frame", "cursor-crosshair",
                                         on_mouse=lambda event: _overview_click(event), events=["click"])
            with ui.column().classes("w-full min-w-0 gap-1"):
                with ui.row().classes("hint-row"):
                    ui.label("Magnifier · click to centre").classes("text-xs muted")
                    info(f"ROI Gr photosites, one image pixel each, enlarged without smoothing (the centre "
                         f"{focus.LOUPE_MAX_PX} × {focus.LOUPE_MAX_PX} of a larger ROI). A smaller ROI magnifies more.")
                loupe = preview_image("zoom_in", "No magnified ROI", "focus-loupe cursor-crosshair",
                                      on_mouse=lambda event: _loupe_click(event), events=["click"])
        with ui.column().classes("plot-surface w-full"):
            chart = ui.echart(_chart_options()).classes("w-full h-48")

    seen: dict = {"version": None, "phase": gox_focus.view().phase}

    def _start() -> None:
        try:
            wanted = request(True)
        except (TypeError, ValueError) as error:
            ui.notify(str(error), type="warning")
            return
        try:
            gox_focus.start(wanted)
        except Exception as error:
            ui.notify(f"Live focus did not start: {error}", type="negative", multi_line=True)
        refresh()

    def _set_half(value) -> None:
        if value is None:
            return
        gox_focus.set_roi_half(int(value))
        app.storage.general["gox_focus_roi_half"] = gox_focus.view().roi_half

    def _overview_click(event) -> None:
        view = gox_focus.view()
        if view.overview_url:
            sx, sy = view.overview_scale
            gox_focus.set_roi_center(event.image_x * sx, event.image_y * sy)

    def _loupe_click(event) -> None:
        view = gox_focus.view()
        if view.loupe_url:
            ox, oy = view.loupe_origin  # one magnifier pixel = one Gr photosite = 2 full-resolution px
            gox_focus.set_roi_center(ox + event.image_x * 2.0, oy + event.image_y * 2.0)

    def _warnings(view: gox_focus.FocusView) -> str:
        parts = []
        if view.clipped_pct is not None and view.clipped_pct > _CLIPPED_WARN_PCT:
            parts.append(f"ROI {view.clipped_pct:.1f} % clipped: lower the exposure.")
        if view.seq is not None and view.score is None and view.signal_dn is not None:
            parts.append(f"ROI too dark ({view.signal_dn:.1f} DN above black): raise the exposure.")
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
        gox_focus.touch()
        view = gox_focus.view()
        ip = target_ip()
        start_btn.set_enabled(view.phase == "idle" and camera_operations.guard_reason("gox") is None
                              and not STATE.snapshot_busy and bool(ip))
        stop_btn.set_enabled(view.phase in ("starting", "live"))
        reset_btn.set_enabled(view.peak is not None)
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

        pct = view.pct
        if pct is None:
            pct_label.set_text("too dark" if view.seq is not None and view.score is None else "—")
            pct_label.classes(replace="focus-percent")
        else:
            tone = "focus-good" if pct >= _GOOD_PCT else "focus-near" if pct >= _NEAR_PCT else "focus-far"
            pct_label.set_text(f"{pct:5.1f} %")
            pct_label.classes(replace=f"focus-percent {tone}")
        score_label.set_text("—" if view.current is None else f"{view.current:.4g}")
        peak_label.set_text("—" if view.peak is None else f"{view.peak:.4g}")
        frame_label.set_text("—" if view.seq is None else f"#{view.seq} · {view.fps:.1f} fps")

        series = chart.options["series"]
        series[0]["data"] = [list(point) for point in view.history]
        series[1]["data"] = ([[view.history[0][0], view.peak], [view.history[-1][0], view.peak]]
                             if view.history and view.peak is not None else [])
        chart.update()

        overview.set_source(view.overview_url)
        loupe.set_source(view.loupe_url)
        overview.set_content(_roi_svg(view))
        loupe.set_content(_loupe_svg(view))

    # No synchronous first refresh: the caller's callbacks may be defined after this panel is
    # built; the timer first fires once the page is connected.
    ui.timer(UI_TICK_S, refresh)
