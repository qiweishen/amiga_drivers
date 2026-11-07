"""Browse acquisition metadata and a selected recording's static AsteRx route."""

from __future__ import annotations

import asyncio
from datetime import date
from nicegui import ui
from pathlib import Path

from . import layout
from ..constants import DRIVERS, REPO_ROOT
from ..services import config_store
from ..services.session_catalog import Catalog
from ..services.session_track import TrackView
from ..state import STATE, ProcState

_STATUS_COLORS = {
    "active": "positive",
    "completed": "grey",
    "stopped": "grey",
    "failed": "negative",
    "unfinished": "warning",
    "unknown": "warning",
}


@ui.page("/sessions")
def sessions_page() -> None:
    catalog = Catalog()
    track = TrackView()
    page = {"number": 0, "track_request": 0}

    def close() -> None:
        catalog.close()
        track.close()

    ui.context.client.on_delete(close)
    try:
        default = config_store.main_settings()["output_dir"] or REPO_ROOT / "data"
    except Exception:
        default = REPO_ROOT / "data"
    with layout.frame("Recording history"):
        with ui.column().classes("console-panel w-full gap-4"):
            with ui.row().classes("console-toolbar w-full"):
                root = ui.input("Recording root on GUI server", value=str(default)).classes("flex-1 min-w-0").props(
                    "outlined dense")
                refresh_btn = ui.button("Refresh", icon="refresh", on_click=lambda: load())
                spinner = ui.spinner()
            with ui.row().classes("console-toolbar filter-bar w-full"):
                after = ui.input("From date", placeholder="YYYY-MM-DD").classes("w-44").props("outlined dense")
                before = ui.input("Through date", placeholder="YYYY-MM-DD").classes("w-44").props("outlined dense")
                sensor = ui.select({"all": "All devices", **{d: d for d in DRIVERS}}, value="all",
                                   label="Device").classes("w-44").props("outlined dense")
                status = ui.select({"all": "All states", "completed": "Completed", "stopped": "Stopped",
                                    "failed": "Failed", "unfinished": "Unfinished", "unknown": "Unknown",
                                    "active": "Active"}, value="all", label="State").classes("w-44").props(
                    "outlined dense")
                ui.button("Filter", icon="filter_list", on_click=lambda: render(reset=True)).props("outline")
            note = ui.label().classes("section-copy")
        ui.label("Completion is the recorded manifest result, not an independent raw-data integrity audit. "
                 "Unfinished recordings remain visible.").classes(
            "section-copy")
        rows = ui.column().classes("w-full gap-3")
        with ui.row().classes("console-toolbar w-full justify-between"):
            previous = ui.button("Previous", on_click=lambda: move(-1)).props("flat")
            paging = ui.label().classes("section-copy mono")
            next_btn = ui.button("Next", on_click=lambda: move(1)).props("flat")

        with ui.column().classes("console-panel w-full gap-3"):
            with ui.row().classes("console-toolbar w-full justify-between"):
                track_title = ui.label("Recorded route").classes("section-title")
                fit_btn = ui.button("Fit route extent", icon="zoom_out_map", on_click=lambda: fit_track()).props(
                    "outline")
                fit_btn.disable()
            track_status = ui.label("Select View route on a stopped recording to inspect its recorded extent.").classes(
                "section-copy whitespace-pre-line")
            track_map = ui.leaflet(center=(0.0, 0.0), zoom=2).classes("plot-surface w-full h-96")
            route_layer = track_map.generic_layer(name="polyline", args=[[], {"color": "#087f8c", "weight": 3}])
            point_layers = []
            ui.label("Pan or zoom to inspect the recorded positions. Long routes may be sampled for display; "
                     "missing or invalid samples and host-time gaps are not connected. "
                     "The route extent describes recorded positions, not sensor field-of-view coverage.").classes(
                "section-copy")

        def clear_track() -> None:
            route_layer.args[0] = []
            if track_map.is_initialized:
                track_map.run_layer_method(route_layer.id, "setLatLngs", [])
            for layer in point_layers:
                track_map.remove_layer(layer)
            point_layers.clear()
            fit_btn.disable()

        def fit_track() -> None:
            if track.result is not None and track_map.is_initialized and not track.closed:
                track_map.run_map_method("fitBounds", track.result.bounds, {"padding": [24, 24], "maxZoom": 18})

        async def show_track(session: Path) -> None:
            page["track_request"] += 1
            request = page["track_request"]
            clear_track()
            track_title.set_text(f"Recorded route · {session.name}")
            track_status.set_text("Reading recorded positions…")
            if not await track.load(session) or track.closed or request != page["track_request"]:
                return
            result = track.result
            if result is None:
                track_status.set_text(f"Route unavailable: {track.error}")
                return
            try:
                await asyncio.wait_for(track_map.initialized(), timeout=10.0)
            except asyncio.TimeoutError:
                if not track.closed and request == page["track_request"]:
                    track_status.set_text("The route was read, but the map did not become ready. Reload this page to retry.")
                return
            if track.closed or request != page["track_request"]:
                return
            lines = [[list(point) for point in segment] for segment in result.segments if len(segment) > 1]
            route_layer.args[0] = lines
            track_map.run_layer_method(route_layer.id, "setLatLngs", lines)
            for segment in result.segments:
                if len(segment) == 1:
                    point_layers.append(track_map.generic_layer(
                        name="circleMarker", args=[segment[0], {"radius": 3, "color": "#087f8c"}]))
            (south, west), (north, east) = result.bounds
            detail = (f"{result.valid_points:,} valid positions · {result.displayed_points:,} displayed points · "
                      f"{result.breaks:,} route breaks · {result.skipped_rows:,} invalid rows · "
                      f"{result.partial_rows:,} incomplete tail rows\n"
                      f"Recorded extent: latitude {south:.7f}…{north:.7f}, longitude {west:.7f}…{east:.7f}.")
            if result.simplified:
                detail += " Display points were sampled; extent uses every valid position."
            track_status.set_text(detail)
            fit_btn.enable()
            fit_track()

        def render(reset: bool = False) -> None:
            if catalog.closed:
                return
            if reset:
                page["number"] = 0
            try:
                lower = date.fromisoformat(after.value).strftime("%Y%m%d") if after.value else ""
                upper = date.fromisoformat(before.value).strftime("%Y%m%d") if before.value else "99999999"
                if lower > upper:
                    raise ValueError("From date must precede Through date")
            except ValueError as exc:
                ui.notify(str(exc), type="warning")
                return
            selected = [r for r in catalog.rows if lower <= r.path.name[:8] <= upper
                        and (sensor.value == "all" or sensor.value in r.drivers)
                        and (status.value == "all" or status.value == r.status)]
            pages = max(1, (len(selected) + 19) // 20)
            page["number"] = min(page["number"], pages - 1)
            rows.clear()
            with rows:
                for record in selected[page["number"] * 20:(page["number"] + 1) * 20]:
                    with ui.card().classes("console-panel record-row w-full gap-2").props("flat"):
                        with ui.row().classes("w-full items-center justify-between"):
                            ui.label(record.path.name).classes("font-bold mono break-all")
                            ui.badge(record.status, color=_STATUS_COLORS.get(record.status, "warning"))
                            route_btn = ui.button("View route", icon="route",
                                                  on_click=lambda _, path=record.path: show_track(path)).props("flat")
                            route_btn.set_enabled(record.status != "active")
                        duration = f" · {record.duration_s:.1f}s" if record.duration_s is not None else ""
                        ui.label(f"{record.started} · {', '.join(record.drivers) or 'devices unknown'}{duration}")
                        ui.label(record.detail).classes("section-copy")
                        ui.label(str(record.path)).classes("text-xs mono muted break-all")
                if not selected:
                    with ui.column().classes("empty-state w-full items-center gap-2"):
                        ui.icon("folder_open").classes("text-3xl muted")
                        ui.label("No matching sessions").classes("font-medium")
                        ui.label("Adjust the date, device or state filters, or refresh the recording root.").classes(
                            "section-copy")
            paging.set_text(f"{len(selected)} matches · page {page['number'] + 1} / {pages}")
            previous.set_enabled(page["number"] > 0)
            next_btn.set_enabled(page["number"] + 1 < pages)
            note.set_text(catalog.note)

        def move(delta: int) -> None:
            page["number"] = max(0, page["number"] + delta)
            render()

        async def load() -> None:
            if catalog.busy:
                return
            refresh_btn.disable()
            spinner.set_visibility(True)
            active = STATE.active_session if STATE.process_state in (ProcState.STARTING, ProcState.RUNNING,
                                                                     ProcState.STOPPING) else None
            try:
                path = Path(root.value or "").expanduser()
                await catalog.load(path if path.is_absolute() else REPO_ROOT / path, active)
                render(reset=True)
            finally:
                if not catalog.closed:
                    refresh_btn.enable()
                    spinner.set_visibility(False)

        spinner.set_visibility(False)
        ui.timer(0.1, load, once=True)
