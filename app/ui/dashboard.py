"""Dashboard: sensor cards, disk gauge, start/stop controls, session info."""

from __future__ import annotations

import time
import json

from nicegui import ui

from ..constants import DRIVERS, UI_TICK_S
from ..services import config_store, config_actions, process
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
        with ui.expansion("Recent control commands", icon="history").classes("w-full"):
            ui.label("A disconnected request can still have executed. Use its ID and current device state before retrying.").classes("text-xs text-gray-600")
            commands = ui.table(columns=[{"name": name, "label": name.replace("_", " ").title(),
                                           "field": name, "align": "left"}
                                          for name in ("id", "action", "state", "error")],
                                rows=[], row_key="id", pagination=8).classes("w-full")
        shown_commands = [None]
        with ui.expansion("Recorded run result and final driver counters", icon="description").classes("w-full"):
            ui.label("These are the acquisition's recorded values; they do not constitute an independent integrity audit.").classes("text-xs text-gray-600")
            run_result = ui.label().classes("text-xs font-mono whitespace-pre-wrap max-h-80 overflow-auto w-full")
        shown_result = [None]

        # --- enable switches -------------------------------------------------
        ui.separator()
        ui.label("Sensor enables (written to config-main.yaml)").classes("font-bold")
        with ui.row().classes("gap-6"):
            switches: dict[str, ui.switch] = {}
            for driver in DRIVERS:
                switches[driver] = ui.switch(
                    {"lms4xxx": "LMS4xxx", "gox": "GoX", "asterx": "AsteRx", "fx10": "FX10"}[driver],
                    on_change=lambda e, d=driver: _on_toggle(d, e.value),
                )

        # --- sensor cards + disk --------------------------------------------
        ui.separator()

        # Built once, updated in place on every tick. Only the card ROW is
        # rebuilt, and only when the sensor set itself changes (a new session
        # can bring different LMS instances) — a clear() per tick would destroy
        # and recreate the "View logs" link below and drop clicks landing in
        # the swap window (same rule as layout.py's banner).
        cards_row = ui.row().classes("gap-4 flex-wrap")
        update_disk = components.disk_gauge()
        with ui.row().classes("items-center gap-2 bg-red-50 text-red-900 px-3 py-2 rounded w-full") as error_row:
            ui.icon("error")
            error_label = ui.label().classes("text-sm break-all")
            ui.link("View logs", "/logs").classes("text-sm")
        error_row.set_visibility(False)

        card_updates: dict[str, object] = {}
        sensor_keys: list[tuple] = [()]

        def rebuild_cards() -> None:
            cards_row.clear()
            card_updates.clear()
            with cards_row:
                for st in STATE.sensors.values():
                    card_updates[st.key] = components.sensor_card(st)

        def refresh_cards() -> None:
            keys = tuple(STATE.sensors)
            if keys != sensor_keys[0]:
                sensor_keys[0] = keys
                rebuild_cards()
            for key, update in card_updates.items():
                st = STATE.sensors.get(key)
                if st is not None:
                    update(st)
            update_disk()
            error_label.set_text(STATE.last_error)
            error_row.set_visibility(bool(STATE.last_error))

        refresh_cards()

        def refresh() -> None:
            if shown_result[0] != STATE.run_result:
                run_result.set_text(json.dumps(STATE.run_result, indent=2, ensure_ascii=False) if STATE.run_result else "No recorded result available")
                shown_result[0] = STATE.run_result
            recent = STATE.recent_commands or ([{
                "id": STATE.tool_operation.get("id", ""), "action": STATE.tool_operation.get("kind", ""),
                "state": STATE.tool_operation.get("state", ""), "error": STATE.tool_operation.get("error", "")
            }] if STATE.tool_operation else [])
            if shown_commands[0] != recent:
                commands.rows = recent
                commands.update()
                shown_commands[0] = recent
            running = STATE.process_state in (ProcState.RUNNING, ProcState.STARTING)
            start_btn.set_enabled(STATE.env_ok and not STATE.control_uncertain and not STATE.snapshot_busy and not STATE.tool_uncertain
                                  and not running and STATE.process_state is not ProcState.STOPPING)
            stop_btn.set_enabled(STATE.env_ok and running)
            for sw in switches.values():
                sw.set_enabled(STATE.env_ok and not STATE.config_locked and not STATE.control_uncertain
                               and not STATE.snapshot_busy and not STATE.tool_uncertain)
            if STATE.active_session is not None:
                started = STATE.session_started_at or 0
                elapsed = max(0, int(time.time() - started))
                session_label.set_text(
                    f"Session {STATE.active_session.name} · elapsed {elapsed // 3600:02d}:{elapsed % 3600 // 60:02d}:{elapsed % 60:02d}"
                    if running
                    else f"Last session {STATE.active_session.name}"
                )
            elif STATE.process_state is ProcState.STARTING:
                session_label.set_text("Waiting for verified session metadata and sensor initialization ...")
            elif STATE.process_state is ProcState.STOPPING:
                session_label.set_text("Stop requested — waiting for shutdown ...")
            else:
                session_label.set_text("")
            refresh_cards()

        def refresh_switches() -> None:
            try:
                enables = config_store.main_settings()["enables"]
            except Exception:
                return
            for driver, sw in switches.items():
                if sw.value != enables.get(driver, False):
                    sw.set_value(enables.get(driver, False))

        refresh_switches()
        # See constants.UI_TICK_S for how this composes with the log flush and
        # tail stages. The tick is cheap because nothing is rebuilt and NiceGUI
        # drops setter calls that would not change anything.
        ui.timer(UI_TICK_S, refresh)
        # Config file re-read + YAML parse: only needs to catch edits made
        # outside this tab.
        ui.timer(5.0, refresh_switches)


async def _on_toggle(driver: str, value: bool) -> None:
    try:
        current = config_store.main_settings()["enables"].get(driver, False)
        if current == value:
            return  # programmatic sync, not a user change
        if STATE.process_state is ProcState.STARTING:
            # The enable set was snapshotted at start; a write now would make
            # the binary load a config the GUI did not capture.
            ui.notify("Starting — please change sensor enables after startup finishes", type="warning")
            return
        await config_actions.change("set_enable", driver, value)
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
        if await process.start():
            ui.notify("Start accepted — waiting for sensor initialization", type="info")
        elif STATE.stop_requested or STATE.process_state is ProcState.EXITED:
            ui.notify("Start cancelled by the stop request", type="info")
        else:
            ui.notify(STATE.last_error or "Start was not accepted; check recording and camera-tool state",
                      type="warning", multi_line=True)
    finally:
        _start_in_flight = False


async def _confirm_stop() -> None:
    generation = STATE.controller_id, STATE.session_generation
    with ui.dialog() as dialog, ui.card():
        ui.label("Stop recording? All sensors shut down together in order.")
        with ui.row():
            ui.button("Stop", color="negative", on_click=lambda: dialog.submit(True))
            ui.button("Cancel", on_click=lambda: dialog.submit(False)).props("flat")
    if await dialog:
        if generation != (STATE.controller_id, STATE.session_generation):
            ui.notify("The recording changed while this dialog was open; review the current session first",
                      type="warning")
            return
        try:
            await process.stop()
            ui.notify("Stop requested — waiting for the acquisition to shut down", type="info")
        except Exception as exc:
            ui.notify(f"Stop request is not confirmed: {exc}", type="negative", multi_line=True)
