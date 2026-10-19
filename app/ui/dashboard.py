"""Dashboard: sensor cards, disk gauge, start/stop controls, session info."""

from __future__ import annotations

import time
import json
from collections.abc import Callable

from nicegui import ui

from ..constants import DRIVERS, UI_TICK_S
from ..services import config_store, config_actions, process
from ..services.storage import human_bytes
from ..state import STATE, ProcState, SensorState
from . import components, layout, recording_settings


def _driver_of(sensor_key: str) -> str:
    return "lms4xxx" if sensor_key.startswith("lms:") else sensor_key


@ui.page("/")
def dashboard_page() -> None:
    enabled_drivers: dict[str, bool] = {}
    syncing_switches = False

    async def start_recording() -> None:
        if not details.ready:
            ui.notify("Save or reload the recording details before starting.", type="warning")
            return
        details.set_enabled(False)
        try:
            await _on_start(lambda: details.ready)
        finally:
            refresh()

    def toggle_driver(driver: str, value: bool):
        # Inspect the guard synchronously: a coroutine would run only after
        # set_value has returned and the programmatic-sync guard is cleared.
        if syncing_switches:
            return
        return save_driver(driver, value)

    async def save_driver(driver: str, value: bool) -> None:
        await _on_toggle(driver, value)
        # Read after the explicit action, including failures, to restore the
        # saved switch values. Metadata drafts are independent of the switches.
        refresh_settings(load_details=False)
        refresh()

    with layout.frame("Overview"):
        with ui.row().classes("console-notice notice-danger").props('role="status"') as error_row:
            ui.icon("error_outline")
            error_label = ui.label().classes("notice-copy")
            ui.link("View logs", "/logs")
        error_row.set_visibility(False)

        with ui.column().classes("recording-console"):
            with ui.row().classes("recording-command-bar"):
                with ui.column().classes("recording-session"):
                    session_caption = ui.label("Session").classes("eyebrow")
                    session_label = ui.label("No active session").classes("session-id mono")
                with ui.row().classes("recording-buttons"):
                    start_btn = ui.button("Start recording", icon="play_arrow", on_click=start_recording)
                    stop_btn = ui.button("Stop recording", icon="stop", color="negative", on_click=_confirm_stop).props("outline")
                    start_btn.disable()
                    stop_btn.disable()
            control_reason = ui.label().classes("control-reason w-full")
            ui.label("Recording continues when this page is closed.").classes("control-note")

        # Session totals and driver telemetry; not an integrity certification.
        with ui.element("div").classes("overview-metrics"):
            with ui.column().classes("metric-cell"):
                ui.label("Running / listed").classes("eyebrow")
                running_value = ui.label("—").classes("metric-value")
                running_value.tooltip("Running status entries / entries selected in Enabled drivers")
            with ui.column().classes("metric-cell"):
                ui.label("Session data").classes("eyebrow")
                bytes_value = ui.label("—").classes("metric-value")
                bytes_value.tooltip("Total files across all sensors in the current or last session")
            with ui.column().classes("metric-cell"):
                ui.label("File growth").classes("eyebrow")
                rate_value = ui.label("—").classes("metric-value")
                rate_value.tooltip("Aggregate file growth across all sensor directories")
            with ui.column().classes("metric-cell"):
                ui.label("Elapsed").classes("eyebrow")
                elapsed_value = ui.label("—").classes("metric-value")
                elapsed_value.tooltip("Host elapsed time of the active recording")

        with ui.element("div").classes("overview-workspace"):
            with ui.column().classes("overview-primary"):
                with ui.column().classes("console-panel sensor-panel"):
                    with ui.row().classes("section-heading items-center"):
                        ui.label("Sensor overview").classes("section-title")
                        channel_label = ui.label().classes("channel-count")
                    # Only membership changes rebuild the rows; telemetry
                    # updates stable elements without dropping focus or clicks.
                    cards_row = ui.element("div").classes("sensor-grid")
                    with ui.column().classes("empty-state") as empty_sensors:
                        ui.icon("sensors")
                        empty_title = ui.label().classes("section-title")
                        empty_copy = ui.label()
                with ui.column().classes("console-panel storage-panel"):
                    update_disk = components.disk_gauge()
                    ui.link("Recording history", "/sessions").classes("storage-history")

            with ui.column().classes("overview-rail"):
                with ui.column().classes("console-panel setup-panel"):
                    with ui.row().classes("section-heading items-center"):
                        ui.label("Next recording").classes("section-title")
                        ui.link("All settings", "/config").classes("text-xs")
                    enable_notice = ui.label().classes("text-xs text-amber-700 break-all")
                    enable_notice.set_visibility(False)
                    details = recording_settings.RecordingSettings(on_pending=start_btn.disable)
                    ui.separator()
                    ui.label("Enabled drivers").classes("eyebrow")
                    ui.label("Switches save immediately for the next start.").classes("section-copy")
                    with ui.element("div").classes("enable-grid"):
                        switches: dict[str, ui.switch] = {}
                        labels = {
                            "asterx": ("AsteRx", "GNSS / IMU"),
                            "fx10": ("FX10", "Hyperspectral camera"),
                            "gox": ("GoX", "RGB camera"),
                            "lms4xxx": ("LMS4xxx", "2D LiDAR"),
                        }
                        for driver in DRIVERS:
                            name, description = labels[driver]
                            with ui.row().classes("enable-option"):
                                switches[driver] = ui.switch(
                                    name, on_change=lambda e, d=driver: toggle_driver(d, e.value),
                                ).props("dense")
                                switches[driver].disable()
                                ui.label(description).classes("enable-detail")

        with ui.column().classes("overview-diagnostics"):
            with ui.expansion("Control command history", icon="history").classes("w-full"):
                ui.label("Check the command ID and device state before retrying a disconnected request.").classes("section-copy")
                commands = ui.table(
                    columns=[{"name": name, "label": name.replace("_", " ").title(),
                              "field": name, "align": "left"}
                             for name in ("id", "action", "state", "error")],
                    rows=[], row_key="id", pagination=8,
                ).classes("w-full").props("dense flat")
            with ui.expansion("Session result and driver counters", icon="description").classes("w-full"):
                ui.label("Recorded driver results; these do not independently verify data integrity.").classes("section-copy")
                run_result = ui.label().classes("terminal-surface whitespace-pre-wrap max-h-80 overflow-auto w-full")

        shown_commands = [None]
        shown_result = [None]
        card_updates: dict[str, object] = {}
        sensor_keys: list[tuple] = [()]

        def visible_sensor_keys() -> tuple[str, ...]:
            return tuple(key for key in STATE.sensors if enabled_drivers.get(_driver_of(key), False))

        def rebuild_cards(keys: tuple[str, ...]) -> None:
            cards_row.clear()
            card_updates.clear()
            with cards_row:
                for key in keys:
                    st = STATE.sensors[key]
                    card_updates[st.key] = components.sensor_card(st)

        def refresh_cards() -> None:
            keys = visible_sensor_keys()
            if keys != sensor_keys[0]:
                sensor_keys[0] = keys
                rebuild_cards(keys)
            for key, update in card_updates.items():
                st = STATE.sensors.get(key)
                if st is not None:
                    update(st)
            empty_sensors.set_visibility(not keys)
            has_enabled = any(enabled_drivers.values())
            empty_title.set_text("Waiting for enabled sensors" if has_enabled else "No drivers enabled")
            empty_copy.set_text("Status entries will appear when an enabled sensor reports its state."
                                if has_enabled else "Enable a driver in Next recording to show its sensors here.")
            channel_label.set_text(f"Status entries: {len(keys)}")
            update_disk()
            error_label.set_text(STATE.last_error)
            error_row.set_visibility(bool(STATE.last_error))

        def refresh() -> None:
            if shown_result[0] != STATE.run_result:
                run_result.set_text(json.dumps(STATE.run_result, indent=2, ensure_ascii=False) if STATE.run_result else "No recorded result available")
                shown_result[0] = STATE.run_result
            recent = ([{
                "id": STATE.tool_operation.get("id", ""), "action": STATE.tool_operation.get("kind", ""),
                "state": STATE.tool_operation.get("state", ""), "error": STATE.tool_operation.get("error", "")
            }] if STATE.tool_operation else [])
            if shown_commands[0] != recent:
                commands.rows = recent
                commands.update()
                shown_commands[0] = recent
            running = STATE.process_state in (ProcState.RUNNING, ProcState.STARTING)
            start_btn.set_enabled(STATE.env_ok and not STATE.control_uncertain and not STATE.snapshot_busy and not STATE.tool_uncertain
                                  and not running and STATE.process_state is not ProcState.STOPPING
                                  and not _start_in_flight and details.ready)
            stop_btn.set_enabled(STATE.env_ok and running)
            details.set_enabled(STATE.env_ok and not STATE.config_locked and not STATE.control_uncertain
                                and not STATE.snapshot_busy and not STATE.tool_uncertain and not _start_in_flight)
            for sw in switches.values():
                sw.set_enabled(STATE.env_ok and not STATE.config_locked and not STATE.control_uncertain
                               and not STATE.snapshot_busy and not STATE.tool_uncertain)

            if not STATE.env_ok:
                reason = "Device controls are unavailable. Review the runtime notice above."
            elif STATE.control_uncertain:
                reason = "Acquisition state is unverified. Waiting for the controller."
            elif STATE.snapshot_busy or STATE.tool_uncertain:
                reason = "A camera tool is active or its state is unverified."
            elif STATE.process_state is ProcState.STARTING:
                reason = "Starting — waiting for verified sensor initialization."
            elif STATE.process_state is ProcState.STOPPING:
                reason = "Stopping — waiting for the acquisition to shut down."
            elif not running and not details.ready:
                reason = "Recording details are not ready. Save or reload changes in Next recording."
            else:
                reason = ""
            control_reason.set_text(reason)
            control_reason.set_visibility(bool(reason))
            session_caption.set_text("Current session" if running or STATE.process_state is ProcState.STOPPING else "Last session")
            session_label.set_text(STATE.active_session.name if STATE.active_session else "No session available")
            started = STATE.session_started_at
            if running and started is not None and started > 0:
                elapsed = max(0, int(time.time() - started))
                elapsed_value.set_text(f"{elapsed // 3600:02d}:{elapsed % 3600 // 60:02d}:{elapsed % 60:02d}")
            else:
                elapsed_value.set_text("—")

            sensors = tuple(STATE.sensors.values())
            visible_sensors = tuple(STATE.sensors[key] for key in visible_sensor_keys())
            running_count = sum(s.state is SensorState.RUNNING for s in visible_sensors)
            running_value.set_text(f"{running_count:02d} / {len(visible_sensors):02d}")
            has_sample = (STATE.storage.generation == STATE.session_generation
                          and STATE.storage.sampled_at > 0 and not STATE.storage.error)
            bytes_value.set_text(human_bytes(sum(s.bytes_total for s in sensors))
                                 if has_sample and sensors and STATE.active_session else "—")
            rate = sum(s.bytes_per_s for s in sensors)
            rate_value.set_text(f"{human_bytes(rate)}/s" if has_sample and sensors and running and rate > 0 else "—")
            refresh_cards()

        def sync_switches() -> None:
            nonlocal syncing_switches
            syncing_switches = True
            try:
                for driver, sw in switches.items():
                    if sw.value != enabled_drivers.get(driver, False):
                        sw.set_value(enabled_drivers.get(driver, False))
            finally:
                syncing_switches = False

        def refresh_settings(*, load_details: bool = True) -> None:
            try:
                settings = config_store.main_settings()
            except Exception as exc:
                enable_notice.set_text(f"Cannot read main configuration: {exc}")
                enable_notice.set_visibility(True)
                if load_details:
                    details.read_failed(exc)
                sync_switches()
                return
            enable_notice.set_visibility(False)
            if load_details:
                details.sync(settings)
            enabled_drivers.update(settings["enables"])
            sync_switches()

        refresh_settings()
        refresh()
        ui.timer(UI_TICK_S, refresh)


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


async def _on_start(settings_ready: Callable[[], bool] | None = None) -> None:
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
        # Preflight and its warning dialog yield. Do not start with stale
        # metadata if an input event arrived while either was in progress.
        if settings_ready is not None and not settings_ready():
            ui.notify("Recording details changed during preflight; save or reload them before starting.",
                      type="warning")
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
    generation = STATE.session_generation
    with ui.dialog() as dialog, ui.card():
        ui.label("Stop recording? All sensors shut down together in order.")
        with ui.row():
            ui.button("Stop", color="negative", on_click=lambda: dialog.submit(True))
            ui.button("Cancel", on_click=lambda: dialog.submit(False)).props("flat")
    if await dialog:
        if generation != STATE.session_generation:
            ui.notify("The recording changed while this dialog was open; review the current session first",
                      type="warning")
            return
        try:
            await process.stop()
            ui.notify("Stop requested — waiting for the acquisition to shut down", type="info")
        except Exception as exc:
            ui.notify(f"Stop request is not confirmed: {exc}", type="negative", multi_line=True)
