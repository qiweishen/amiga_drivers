"""Dashboard: sensor cards, disk gauge, start/stop controls, session info."""

from __future__ import annotations

import json
import time
from collections.abc import Callable
from nicegui import ui

from . import components, layout, recording_settings
from ..constants import DRIVERS, UI_TICK_S
from ..services import config_store, process
from ..services.storage import human_bytes
from ..state import STATE, ProcState, SensorState


_DRIVER_LABELS = {
    "asterx": ("AsteRx", "GNSS / IMU"),
    "fx10": ("FX10", "Hyperspectral camera"),
    "gox": ("GoX", "RGB camera"),
    "lms4xxx": ("LMS4xxx", "2D LiDAR"),
}


def _driver_of(sensor_key: str) -> str:
    return "lms4xxx" if sensor_key.startswith("lms:") else sensor_key


def _cameras_free() -> bool:
    """No unverified controller state and no camera tool holding a device."""
    return not STATE.control_uncertain and not STATE.snapshot_busy and not STATE.tool_uncertain


def _control_reason(running: bool, details_ready: bool) -> str:
    if STATE.control_uncertain:
        return "Acquisition state unverified; waiting for the controller."
    if STATE.snapshot_busy or STATE.tool_uncertain:
        return "A camera tool is active."
    if STATE.process_state is ProcState.STARTING:
        return "Starting: waiting for sensor initialization."
    if STATE.process_state is ProcState.STOPPING:
        return "Stopping: waiting for shutdown."
    if not running and not details_ready:
        return "Save or reload the recording details first."
    return ""


def _elapsed_text(running: bool) -> str:
    started = STATE.session_started_at
    if not running or started is None or started <= 0:
        return "—"
    elapsed = max(0, int(time.time() - started))
    return f"{elapsed // 3600:02d}:{elapsed % 3600 // 60:02d}:{elapsed % 60:02d}"


def _driver_switches(on_change: Callable[[str, bool], object]) -> dict[str, ui.switch]:
    switches = {}
    with ui.element("div").classes("enable-grid"):
        for driver in DRIVERS:
            name, description = _DRIVER_LABELS[driver]
            with ui.row().classes("enable-option"):
                switches[driver] = ui.switch(name, on_change=lambda e, d=driver: on_change(d, e.value)).props("dense")
                switches[driver].disable()
                ui.label(description).classes("enable-detail")
    return switches


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
                    stop_btn = ui.button("Stop recording", icon="stop", color="negative", on_click=_confirm_stop).props(
                        "outline")
                    start_btn.disable()
                    stop_btn.disable()
            control_reason = ui.label().classes("control-reason w-full")
            ui.label("Recording continues when this page is closed.").classes("control-note")

        # Session totals and driver telemetry; not an integrity certification.
        with ui.element("div").classes("overview-metrics"):
            running_value = components.metric("Running / listed")
            bytes_value = components.metric("Session data")
            rate_value = components.metric("File growth")
            elapsed_value = components.metric("Elapsed")

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
                    components.hint("Saved at once for the next start.")
                    switches = _driver_switches(toggle_driver)

        with ui.column().classes("overview-diagnostics"):
            with ui.expansion("Control command history", icon="history").classes("w-full"):
                components.hint("Check the device state before retrying a disconnected command.")
                commands = ui.table(
                    columns=[{"name": name, "label": name.replace("_", " ").title(),
                              "field": name, "align": "left"}
                             for name in ("id", "action", "state", "error")],
                    rows=[], row_key="id", pagination=8,
                ).classes("w-full").props("dense flat")
            with ui.expansion("Session result and driver counters", icon="description").classes("w-full"):
                components.hint("Driver-reported results, not an integrity audit.")
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
            empty_copy.set_text("Sensors appear once they report."
                                if has_enabled else "Enable a driver under Next recording.")
            channel_label.set_text(f"Status entries: {len(keys)}")
            update_disk()
            error_label.set_text(STATE.last_error)
            error_row.set_visibility(bool(STATE.last_error))

        def refresh() -> None:
            if shown_result[0] != STATE.run_result:
                run_result.set_text(json.dumps(STATE.run_result, indent=2,
                                               ensure_ascii=False) if STATE.run_result else "No recorded result available")
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
            editable = _cameras_free() and not STATE.config_locked
            start_btn.set_enabled(_cameras_free() and not running and STATE.process_state is not ProcState.STOPPING
                                  and not _start_in_flight and details.ready)
            stop_btn.set_enabled(running)
            details.set_enabled(editable and not _start_in_flight)
            for sw in switches.values():
                sw.set_enabled(editable)
            reason = _control_reason(running, details.ready)
            control_reason.set_text(reason)
            control_reason.set_visibility(bool(reason))
            session_caption.set_text(
                "Current session" if running or STATE.process_state is ProcState.STOPPING else "Last session")
            session_label.set_text(STATE.active_session.name if STATE.active_session else "No session available")
            elapsed_value.set_text(_elapsed_text(running))

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
            ui.notify("Change enables after startup finishes", type="warning")
            return
        config_store.set_enable(driver, value)
        if STATE.process_state is ProcState.RUNNING:
            STATE.pending_config_notice = True
            ui.notify(components.NEXT_RUN, type="info")
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
        errors = await process.preflight()
        for e in errors:
            ui.notify(e, type="negative", multi_line=True)
        if errors:
            return
        # Preflight yields: do not start with stale metadata edited meanwhile.
        if settings_ready is not None and not settings_ready():
            ui.notify("Recording details changed; save or reload them first.",
                      type="warning")
            return
        if await process.start():
            ui.notify("Start accepted; initializing sensors", type="info")
        elif STATE.stop_requested or STATE.process_state is ProcState.EXITED:
            ui.notify("Start cancelled by the stop request", type="info")
        else:
            ui.notify(STATE.last_error or "Start was not accepted",
                      type="warning", multi_line=True)
    finally:
        _start_in_flight = False


async def _confirm_stop() -> None:
    generation = STATE.session_generation
    if not await components.confirm("Stop recording? All sensors stop together.", "Stop", color="negative"):
        return
    if generation != STATE.session_generation:
        ui.notify("The recording changed meanwhile; review it first", type="warning")
        return
    try:
        await process.stop()
        ui.notify("Stop requested; waiting for shutdown", type="info")
    except Exception as exc:
        ui.notify(f"Stop request is not confirmed: {exc}", type="negative", multi_line=True)
