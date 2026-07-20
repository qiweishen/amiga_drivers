"""Small reusable UI builders (sensor card, disk gauge)."""

from __future__ import annotations

from nicegui import ui

from ..services.storage import human_bytes
from ..state import STATE, SensorState, SensorStatus

_SENSOR_BADGE = {
    SensorState.DISABLED: ("Disabled", "grey"),
    SensorState.WAITING: ("Initializing", "orange"),
    SensorState.RUNNING: ("Running", "green"),
    SensorState.STOPPED: ("Stopped", "blue-grey"),
    SensorState.FAILED: ("Failed", "red"),
}

_SENSOR_ICON = {
    "ins401": "explore",
    "gox": "photo_camera",
    "asterx": "satellite_alt",
}


def sensor_card(st: SensorStatus, enabled_switch=None) -> None:
    icon = _SENSOR_ICON.get(st.key, "lidar" if st.key.startswith("lms:") else "sensors")
    text, color = _SENSOR_BADGE[st.state]
    with ui.card().classes("w-64"):
        with ui.row().classes("items-center justify-between w-full"):
            with ui.row().classes("items-center gap-2"):
                ui.icon(icon).classes("text-2xl")
                ui.label(st.label).classes("font-bold")
            ui.badge(text).props(f'color="{color}"')
        with ui.row().classes("items-center justify-between w-full text-sm"):
            ui.label(f"Data {human_bytes(st.bytes_total)}")
            rate = f"{human_bytes(st.bytes_per_s)}/s" if st.bytes_per_s > 0 else "—"
            ui.label(f"Rate {rate}")
        if st.last_error:
            ui.label(st.last_error).classes("text-xs text-red-700 break-all")
        if enabled_switch is not None:
            enabled_switch()


def disk_gauge() -> None:
    s = STATE.storage
    if s.disk_total <= 0:
        ui.label("Disk info unavailable (output directory does not exist)").classes("text-sm text-gray-500")
        return
    used_frac = s.disk_used / s.disk_total
    with ui.column().classes("w-full gap-1"):
        with ui.row().classes("items-center justify-between w-full"):
            ui.label("Output disk").classes("font-bold")
            ui.label(f"{human_bytes(s.disk_free)} free of {human_bytes(s.disk_total)}").classes("text-sm")
        ui.linear_progress(value=round(used_frac, 3), show_value=False).props(
            f'color="{"red" if used_frac > 0.9 else "amber" if used_frac > 0.75 else "primary"}"'
        )
