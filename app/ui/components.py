"""Small reusable UI builders (sensor card, disk gauge).

Both builders create their elements ONCE and return an ``update()`` callable.
The obvious alternative — rebuilding the container on every timer tick — costs a
full element teardown per tick *per open browser tab* and drops clicks that land
in the swap window; layout.py's banner carries the same rule. Assigning the same
value to an element is free (NiceGUI's setters are change-guarded), so an idle
tick sends nothing and the refresh interval stays a free parameter.
"""

from __future__ import annotations

from typing import Callable

from nicegui import ui

from ..services.driver_stats import STATS
from ..services.storage import human_bytes
from ..state import STATE, SensorState, SensorStatus


# Brand names (constants.PALETTE), not literal hues, so retheming reaches these.
# grey/blue-grey stay literal: Quasar has no brand slot for a neutral.
_SENSOR_BADGE = {
    SensorState.DISABLED: ("Disabled", "grey"),
    SensorState.WAITING: ("Initializing", "warning"),
    SensorState.RUNNING: ("Running", "positive"),
    SensorState.STOPPED: ("Stopped", "grey"),
    SensorState.FAILED: ("Failed", "negative"),
}

_SENSOR_ICON = {
    "gox": "photo_camera",
    "asterx": "satellite_alt",
}


def sensor_card(st: SensorStatus) -> Callable[[SensorStatus], None]:
    """Build one sensor card; returns update(SensorStatus) for the page timer."""
    icon = _SENSOR_ICON.get(st.key, "lidar" if st.key.startswith("lms:") else "sensors")
    # AsteRx records a byte stream, not frames — it has no write rate to show.
    has_fps = st.key != "asterx"

    with ui.card().classes("w-64"):
        with ui.row().classes("items-center justify-between w-full"):
            with ui.row().classes("items-center gap-2"):
                ui.icon(icon).classes("text-2xl")
                ui.label(st.label).classes("font-bold")
            badge = ui.badge("")
        with ui.row().classes("items-center justify-between w-full text-sm"):
            data_label = ui.label()
            rate_label = ui.label()
        if has_fps:
            # Frames actually written to disk, straight from the driver's own
            # [Statistics] line (services/driver_stats.py).
            with ui.row().classes("items-center justify-between w-full text-sm"):
                ui.label("Written")
                fps_label = ui.label()
                # The GoX card sums its cameras, so it carries a breakdown
                # tooltip. Its text is kept non-empty rather than toggling
                # visibility: hiding a q-tooltip portal is not worth relying on.
                fps_tip = None
                if st.key == "gox":
                    with fps_label:
                        fps_tip = ui.tooltip("")
        error_label = ui.label().classes("text-xs text-red-700 break-all")
        error_label.set_visibility(False)

    def update(s: SensorStatus) -> None:
        text, color = _SENSOR_BADGE[s.state]
        badge.set_text(text)
        badge.props(f'color="{color}"')
        data_label.set_text(f"Data {human_bytes(s.bytes_total)}")
        rate = f"{human_bytes(s.bytes_per_s)}/s" if s.bytes_per_s > 0 else "—"
        rate_label.set_text(f"Rate {rate}")
        if has_fps:
            fps = STATS.write_fps(s.key)
            fps_label.set_text(f"{fps:.1f} fps" if fps is not None else "—")
            if fps_tip is not None:
                breakdown = STATS.gox_breakdown()
                fps_tip.set_text(" · ".join(f"{cam} {v:.1f} fps" for cam, v in breakdown)
                                 if breakdown else "no camera has reported yet")
        error_label.set_text(s.last_error)
        error_label.set_visibility(bool(s.last_error))

    update(st)
    return update


def disk_gauge() -> Callable[[], None]:
    """Build the output-disk gauge; returns update() for the page timer."""
    with ui.column().classes("w-full gap-1") as box:
        with ui.row().classes("items-center justify-between w-full"):
            title = ui.label("Output disk").classes("font-bold")
            free_label = ui.label().classes("text-sm")
        bar = ui.linear_progress(value=0.0, show_value=False)
        path_label = ui.label("").classes("text-xs text-gray-600 break-all")
    next_label = ui.label("").classes("text-xs text-gray-600 break-all")
    error_label = ui.label("").classes("text-sm text-amber-700 break-all")
    missing_label = ui.label("Disk info is pending or unavailable").classes(
        "text-sm text-gray-500"
    )

    def update() -> None:
        s = STATE.storage
        available = s.generation == STATE.session_generation and s.disk_total > 0
        title.set_text(s.disk_scope)
        path_label.set_text(str(s.disk_path or ""))
        next_label.set_text(f"Next recording output: {s.next_output_path or 'unavailable'}")
        error_label.set_text(s.error)
        box.set_visibility(available)
        missing_label.set_visibility(not available)
        if not available:
            return
        used_frac = s.disk_used / s.disk_total
        free_label.set_text(f"{human_bytes(s.disk_free)} free of {human_bytes(s.disk_total)}")
        bar.set_value(round(used_frac, 3))
        bar.props(f'color="{"negative" if used_frac > 0.9 else "warning" if used_frac > 0.75 else "primary"}"')

    update()
    return update
