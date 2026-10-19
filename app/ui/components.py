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
    "fx10": "view_week",
}

_SENSOR_KIND = {
    "gox": "RGB camera",
    "asterx": "GNSS / IMU",
    "fx10": "Hyperspectral",
}


def sensor_card(st: SensorStatus) -> Callable[[SensorStatus], None]:
    """Build one compact sensor row; return its stable-element update callback."""
    icon = _SENSOR_ICON.get(st.key, "lidar" if st.key.startswith("lms:") else "sensors")
    # AsteRx records a byte stream, so a written frame rate does not apply.
    has_fps = st.key != "asterx"

    with ui.element("div").classes("sensor-card") as card:
        with ui.element("div").classes("sensor-row"):
            with ui.row().classes("sensor-identity"):
                ui.icon(icon).classes("sensor-icon")
                with ui.column().classes("gap-0 min-w-0"):
                    name_label = ui.label(st.label).classes("sensor-name")
                    ui.label(_SENSOR_KIND.get(st.key, "2D LiDAR")).classes("sensor-kind")
            with ui.element("div").classes("sensor-state"):
                badge = ui.badge("")
            with ui.column().classes("sensor-measure"):
                ui.label("Recorded bytes").classes("sensor-measure-label")
                data_label = ui.label("—").classes("sensor-reading-value")
            with ui.column().classes("sensor-measure"):
                ui.label("File growth").classes("sensor-measure-label")
                rate_label = ui.label("—").classes("sensor-reading-value")
            with ui.column().classes("sensor-measure"):
                if has_fps:
                    # Driver-reported frames written, not the configured capture rate.
                    ui.label("Written frame rate").classes("sensor-measure-label")
                    fps_label = ui.label("—").classes("sensor-reading-value")
                    # The GoX row sums its cameras, so it carries a breakdown
                    # tooltip. Its text is kept non-empty rather than toggling
                    # visibility: hiding a q-tooltip portal is not worth relying on.
                    fps_tip = None
                    if st.key == "gox":
                        with fps_label:
                            fps_tip = ui.tooltip("")
                else:
                    ui.label("Recording mode").classes("sensor-measure-label")
                    ui.label("Stream").classes("sensor-reading-value sensor-stream")
        error_label = ui.label().classes("sensor-error")
        error_label.set_visibility(False)

    def update(s: SensorStatus) -> None:
        text, color = _SENSOR_BADGE[s.state]
        card.props(f'data-state="{s.state.value}"')
        # Labels can change with a new session; card identity is the sensor key.
        name_label.set_text(s.label)
        badge.set_text(text)
        badge.props(f'color="{color}"')
        has_sample = (STATE.active_session is not None
                      and STATE.storage.generation == STATE.session_generation
                      and STATE.storage.sampled_at > 0 and not STATE.storage.error)
        data_label.set_text(human_bytes(s.bytes_total) if has_sample else "—")
        rate = (f"{human_bytes(s.bytes_per_s)}/s"
                if has_sample and s.state is SensorState.RUNNING and s.bytes_per_s > 0 else "—")
        rate_label.set_text(rate)
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
    title = ui.label("Output disk").classes("section-title storage-heading")
    with ui.column().classes("storage-gauge w-full gap-2") as box:
        with ui.row().classes("storage-capacity"):
            free_label = ui.label().classes("storage-free")
            total_label = ui.label().classes("storage-meta")
            used_label = ui.label().classes("storage-used storage-meta mono")
        bar = ui.linear_progress(value=0.0, show_value=False).classes("w-full")
        path_label = ui.label("").classes("storage-path")
    next_label = ui.label("").classes("storage-path")
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
        next_label.set_visibility(not available or s.next_output_path != s.disk_path)
        error_label.set_text(s.error)
        error_label.set_visibility(bool(s.error))
        box.set_visibility(available)
        missing_label.set_visibility(not available)
        if not available:
            return
        used_frac = s.disk_used / s.disk_total
        free_label.set_text(f"{human_bytes(s.disk_free)} free")
        total_label.set_text(f"of {human_bytes(s.disk_total)}")
        used_label.set_text(f"{used_frac:.0%} used")
        bar.set_value(round(used_frac, 3))
        bar.props(f'color="{"negative" if used_frac > 0.9 else "warning" if used_frac > 0.75 else "primary"}"')

    update()
    return update
