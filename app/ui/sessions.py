"""Historical acquisition sessions, filtered and paginated without reading raw payloads."""

from __future__ import annotations

from datetime import date
from pathlib import Path
from urllib.parse import urlencode

from nicegui import ui

from ..constants import DRIVERS, REPO_ROOT
from ..services import config_store
from ..services.session_catalog import Catalog
from ..state import STATE, ProcState
from . import layout


@ui.page("/sessions")
def sessions_page() -> None:
    catalog = Catalog()
    ui.context.client.on_delete(catalog.close)
    page = {"number": 0}
    try:
        default = config_store.main_settings()["output_dir"] or REPO_ROOT / "data"
    except Exception:
        default = REPO_ROOT / "data"
    with layout.frame("Recording history"):
        with ui.row().classes("w-full items-end"):
            root = ui.input("Recording root on GUI server", value=str(default)).classes("flex-1")
            refresh_btn = ui.button("Refresh", icon="refresh", on_click=lambda: load())
            spinner = ui.spinner()
        with ui.row().classes("items-end gap-3"):
            after = ui.input("From date", placeholder="YYYY-MM-DD")
            before = ui.input("Through date", placeholder="YYYY-MM-DD")
            sensor = ui.select({"all": "All devices", **{d: d for d in DRIVERS}}, value="all", label="Device")
            status = ui.select({"all": "All states", "completed": "Completed", "stopped": "Stopped",
                                "failed": "Failed", "unfinished": "Unfinished", "unknown": "Unknown",
                                "active": "Active"}, value="all", label="State")
            ui.button("Filter", icon="filter_list", on_click=lambda: render(reset=True)).props("outline")
        note = ui.label().classes("text-xs text-gray-600")
        ui.label("Completion is the recorded manifest result, not an independent raw-data integrity audit. "
                 "Unfinished recordings remain visible; finalized records with AsteRx CSVs can be replayed.").classes("text-xs text-gray-600")
        rows = ui.column().classes("w-full gap-2")
        with ui.row().classes("items-center"):
            previous = ui.button("Previous", on_click=lambda: move(-1)).props("flat")
            paging = ui.label()
            next_btn = ui.button("Next", on_click=lambda: move(1)).props("flat")

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
                    with ui.card().classes("w-full gap-1"):
                        with ui.row().classes("w-full items-center justify-between"):
                            ui.label(record.path.name).classes("font-bold")
                            ui.badge(record.status)
                            if record.replay:
                                ui.link("Replay AsteRx", "/asterx?" + urlencode({"session": str(record.path)}))
                        duration = f" · {record.duration_s:.1f}s" if record.duration_s is not None else ""
                        ui.label(f"{record.started} · {', '.join(record.drivers) or 'devices unknown'}{duration}")
                        ui.label(record.detail).classes("text-sm text-gray-600")
                        ui.label(str(record.path)).classes("text-xs font-mono break-all")
                if not selected:
                    ui.label("No matching sessions")
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
            active = STATE.active_session if STATE.process_state in (ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING) else None
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
