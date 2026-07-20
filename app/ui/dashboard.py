"""Dashboard: sensor cards, disk gauge, start/stop controls, session info."""

from __future__ import annotations

import time
from datetime import datetime

from nicegui import ui

from ..constants import DRIVERS
from ..services import config_store, process
from ..state import STATE, ProcState
from . import components, layout


def _driver_of(sensor_key: str) -> str:
    return "lms4xxx" if sensor_key.startswith("lms:") else sensor_key


@ui.page("/")
def dashboard_page() -> None:
    with layout.frame("Overview"):
        # --- control row -----------------------------------------------------
        with ui.row().classes("items-center gap-4"):
            start_btn = ui.button("Start recording", icon="play_arrow", on_click=_on_start)
            stop_btn = ui.button("Stop recording", icon="stop", color="negative", on_click=_confirm_stop)
            session_label = ui.label().classes("text-sm text-gray-600")
        ui.label("Note: closing the GUI does not stop recording; reopening it reattaches automatically.").classes("text-xs text-gray-500")

        # --- enable switches -------------------------------------------------
        ui.separator()
        ui.label("Sensor enables (written to config-main.yaml, applied at start)").classes("font-bold")
        with ui.row().classes("gap-6"):
            switches: dict[str, ui.switch] = {}
            for driver in DRIVERS:
                switches[driver] = ui.switch(
                    {"ins401": "INS401", "lms4xxx": "LMS4xxx", "gox": "GoX", "asterx": "AsteRx"}[driver],
                    on_change=lambda e, d=driver: _on_toggle(d, e.value),
                )

        # --- sensor cards + disk --------------------------------------------
        ui.separator()

        @ui.refreshable
        def cards() -> None:
            with ui.row().classes("gap-4 flex-wrap"):
                for st in STATE.sensors.values():
                    components.sensor_card(st)
            components.disk_gauge()
            if STATE.last_error:
                with ui.row().classes("items-center gap-2 bg-red-50 text-red-900 px-3 py-2 rounded w-full"):
                    ui.icon("error")
                    ui.label(STATE.last_error).classes("text-sm break-all")
                    ui.link("View logs", "/logs").classes("text-sm")

        cards()

        def refresh() -> None:
            running = STATE.process_state in (ProcState.RUNNING, ProcState.STARTING)
            start_btn.set_enabled(STATE.env_ok and not running and STATE.process_state is not ProcState.STOPPING)
            stop_btn.set_enabled(running)
            if STATE.active_session is not None:
                started = STATE.session_started_at or 0
                elapsed = max(0, int(time.time() - started))
                session_label.set_text(
                    f"Session {STATE.active_session.name} · elapsed {elapsed // 3600:02d}:{elapsed % 3600 // 60:02d}:{elapsed % 60:02d}"
                    if running
                    else f"Last session {STATE.active_session.name}"
                )
            elif STATE.process_state is ProcState.STARTING:
                session_label.set_text("Waiting for the session directory…")
            else:
                session_label.set_text("")
            cards.refresh()

        def refresh_switches() -> None:
            try:
                enables = config_store.main_settings()["enables"]
            except Exception:
                return
            for driver, sw in switches.items():
                if sw.value != enables.get(driver, False):
                    sw.set_value(enables.get(driver, False))

        refresh_switches()
        ui.timer(1.0, refresh)
        ui.timer(5.0, refresh_switches)


def _on_toggle(driver: str, value: bool) -> None:
    try:
        current = config_store.main_settings()["enables"].get(driver, False)
        if current == value:
            return  # programmatic sync, not a user change
        if STATE.process_state is ProcState.STARTING:
            # The enable set was snapshotted at start; a write now would make
            # the binary load a config the GUI did not capture.
            ui.notify("Starting — please change sensor enables after startup finishes", type="warning")
            return
        config_store.set_enable(driver, value)
        if STATE.process_state is ProcState.RUNNING:
            STATE.pending_config_notice = True
            ui.notify("Saved — takes effect on the next recording start", type="info")
    except Exception as e:
        ui.notify(f"Failed to write the Enable flag: {e}", type="negative")


_start_in_flight = False  # single-flight guard: preflight awaits several execs


async def _on_start() -> None:
    global _start_in_flight
    if _start_in_flight or STATE.process_state in (
        ProcState.STARTING, ProcState.RUNNING, ProcState.STOPPING
    ):
        return
    _start_in_flight = True
    try:
        errors, warnings = await process.preflight()
        if errors:
            for e in errors:
                ui.notify(e, type="negative", multi_line=True)
            return
        if warnings:
            with ui.dialog() as dialog, ui.card():
                ui.label("Pre-start warnings").classes("font-bold")
                for w in warnings:
                    ui.label("· " + w).classes("text-sm")
                with ui.row():
                    ui.button("Start anyway", on_click=lambda: dialog.submit(True))
                    ui.button("Cancel", on_click=lambda: dialog.submit(False)).props("flat")
            if not await dialog:
                return
        await process.start()
        ui.notify("Recording started", type="positive")
    finally:
        _start_in_flight = False


async def _confirm_stop() -> None:
    with ui.dialog() as dialog, ui.card():
        ui.label("Stop recording? All sensors shut down together in order.")
        with ui.row():
            ui.button("Stop", color="negative", on_click=lambda: dialog.submit(True))
            ui.button("Cancel", on_click=lambda: dialog.submit(False)).props("flat")
    if await dialog:
        await process.stop()
        ui.notify("Stop request sent", type="info")
