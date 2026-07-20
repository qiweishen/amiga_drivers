"""Shared page frame: header navigation + global status chip + warning banners."""

from __future__ import annotations

from contextlib import contextmanager

from nicegui import ui

from ..state import STATE, ProcState
from ..services import docker_runner, runtime

_PROC_BADGE = {
    ProcState.IDLE: ("Idle", "grey"),
    ProcState.STARTING: ("Starting…", "orange"),
    ProcState.RUNNING: ("Recording", "green"),
    ProcState.STOPPING: ("Stopping…", "orange"),
    ProcState.EXITED: ("Stopped", "blue-grey"),
    ProcState.FAILED: ("Crashed", "red"),
}

_NAV = [
    ("/", "Overview"),
    ("/config", "Config"),
    ("/logs", "Logs"),
    ("/gox", "GoX Tools"),
]


@contextmanager
def frame(title: str):
    with ui.header().classes("items-center justify-between bg-primary text-white px-4"):
        with ui.row().classes("items-center gap-6"):
            ui.label("Amiga Sensor Console").classes("text-lg font-bold")
            for path, label in _NAV:
                ui.link(label, path).classes("text-white no-underline hover:underline")
        with ui.row().classes("items-center gap-2"):
            mode_badge = ui.badge("").props('color="blue-grey" outline').classes("px-2 py-1 text-xs")
            badge = ui.badge("").props("floating=false").classes("px-3 py-1 text-sm")

    banner_row = ui.row().classes("w-full")

    banner_key: list[tuple] = [()]  # last rendered banner state

    def refresh_header() -> None:
        mode_badge.set_text({"docker": "Docker", "native": "Native"}.get(STATE.mode, "…"))
        text, color = _PROC_BADGE[STATE.process_state]
        if STATE.process_state is ProcState.RUNNING and not STATE.attached:
            text += " (reattached)"
        badge.set_text(text)
        badge.props(f'color="{color}"')
        # Rebuild the banner only when its state actually changed — a clear()
        # every tick would destroy/recreate the "Start container" button and
        # drop clicks landing in the swap window.
        key = (STATE.env_ok, STATE.env_detail, STATE.mode, STATE.pending_config_notice)
        if key == banner_key[0]:
            return
        banner_key[0] = key
        banner_row.clear()
        if not STATE.env_ok:
            with banner_row:
                with ui.row().classes("w-full items-center bg-red-100 text-red-900 px-4 py-2 rounded"):
                    ui.icon("error")
                    ui.label(f"{STATE.env_detail or 'Runtime environment unavailable'} — all controls are disabled")
                    if STATE.mode == "docker":
                        ui.button("Start container", on_click=_compose_up).props("flat dense")
        elif STATE.pending_config_notice:
            with banner_row:
                with ui.row().classes("w-full items-center bg-amber-100 text-amber-900 px-4 py-2 rounded"):
                    ui.icon("info")
                    ui.label("Saved config changes are pending — they take effect on the next recording start")

    ui.timer(1.0, refresh_header)
    refresh_header()

    with ui.column().classes("w-full max-w-6xl mx-auto p-4 gap-4"):
        ui.label(title).classes("text-2xl font-bold")
        yield


async def _compose_up() -> None:
    ui.notify("Starting the container (the first run may build the image — please wait)…")
    res = await docker_runner.compose_up()
    if res.ok:
        STATE.env_ok, STATE.env_detail = await runtime.env_check()
        ui.notify("Container started", type="positive")
    else:
        ui.notify(f"Failed to start the container: {res.stderr.strip()[-300:]}", type="negative", multi_line=True)
