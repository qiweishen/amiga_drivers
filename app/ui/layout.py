"""Shared console shell, navigation and persistent acquisition status."""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path

from nicegui import ui

from ..constants import UI_TICK_S
from ..state import STATE, ProcState
from ..services import tool_jobs


_STYLES = Path(__file__).resolve().parent.parent / "resource" / "console.css"
_PROC_BADGE = {
    ProcState.IDLE: ("Idle", "grey"),
    ProcState.STARTING: ("Starting", "warning"),
    ProcState.RUNNING: ("Recording", "positive"),
    ProcState.STOPPING: ("Stopping", "warning"),
    ProcState.EXITED: ("Stopped", "grey"),
    ProcState.FAILED: ("Failed / incomplete", "negative"),
}

# Original titles remain stable so each page selects its own navigation item.
_NAV = (
    ("Operations", "/", "Overview", "Overview", "dashboard"),
    ("Operations", "/live", "Data Live", "Data preview", "monitor_heart"),
    ("Operations", "/asterx", "AsteRx · Live & History", "Navigation & IMU", "explore"),
    ("Device setup", "/camera", "Camera Tools", "Camera workbench", "photo_camera"),
    ("Device setup", "/reference", "Collect Reference — FX10", "Reference capture", "contrast"),
    ("Device setup", "/config", "Config", "Configuration", "tune"),
    ("Diagnostics", "/logs", "Logs", "System logs", "terminal"),
    ("Diagnostics", "/sessions", "Recording history", "Recording history", "inventory_2"),
)
_PAGE_CONTEXT = {
    "Data Live": "Previews update on request during recording.",
    "Camera Tools": "Camera discovery and snapshots are available before recording.",
    "Config": "Saved settings apply to the next recording.",
}


@contextmanager
def frame(title: str):
    ui.add_css(_STYLES.read_text(encoding="utf-8"))
    current = next((item for item in _NAV if item[2] == title), None)
    page_name = current[3] if current else title

    with ui.left_drawer(value=None, top_corner=True, bottom_corner=True).props(
        "width=200 breakpoint=1024"
    ).classes("console-sidebar") as drawer:
        with ui.link(target="/").classes("console-brand").props('aria-label="Amiga console overview"'):
            ui.label("AMIGA").classes("brand-name")
            ui.label("Acquisition").classes("brand-subtitle")
        with ui.element("nav").classes("console-nav").props('aria-label="Main navigation"'):
            last_group = ""
            for group, path, original_title, label, icon in _NAV:
                if group != last_group:
                    ui.label(group).classes("nav-group")
                    last_group = group
                active = original_title == title
                with ui.link(target=path).classes("nav-link" + (" nav-link-active" if active else "")) as link:
                    if active:
                        link.props('aria-current="page"')
                    ui.icon(icon).classes("nav-icon")
                    ui.label(label)
        with ui.column().classes("sidebar-footer"):
            ui.label("Controller").classes("sidebar-eyebrow")
            controller_label = ui.label("Status pending").classes("sidebar-controller")
            status_label = ui.label().classes("sidebar-detail")

    with ui.header().classes("console-topbar"):
        with ui.row().classes("topbar-location"):
            ui.button(icon="menu", on_click=drawer.toggle).props(
                'flat round dense aria-label="Toggle navigation"'
            ).classes("nav-toggle")
            ui.label(page_name).classes("topbar-page page-title").props('role="heading" aria-level="1"')
        with ui.row().classes("topbar-status"):
            ui.label("Acquisition").classes("topbar-state-label")
            badge = ui.badge("").classes("state-badge")

    async def stop_tool() -> None:
        operation_id = STATE.tool_operation.get("id", "")
        try:
            await tool_jobs.stop(operation_id)
        except Exception as exc:
            ui.notify(f"Tool stop is not confirmed: {exc}", type="negative", multi_line=True)

    with ui.column().classes("console-main"):
        with ui.column().classes("global-notices") as notices:
            with ui.row().classes("console-notice notice-danger").props('role="status"') as env_notice:
                ui.icon("error_outline")
                env_label = ui.label().classes("notice-copy")
            with ui.row().classes("console-notice notice-warning").props('role="status"') as pending_notice:
                ui.icon("schedule")
                ui.label("Saved configuration changes take effect on the next recording start.").classes("notice-copy")
            with ui.row().classes("console-notice notice-tool") as tool_row:
                ui.icon("build_circle")
                tool_label = ui.label().classes("notice-copy mono")
                tool_stop = ui.button("Stop tool", color="negative", on_click=stop_tool).props("flat dense no-caps")

        if context := _PAGE_CONTEXT.get(title):
            ui.label(context).classes("page-context")

        def refresh_header() -> None:
            operation = STATE.tool_operation
            # Successful, finished jobs remain in command history; banners
            # are reserved for work in progress or results needing attention.
            show_tool = bool(operation) and (operation.get("state") != "completed" or bool(operation.get("error")))
            notices.set_visibility(not STATE.env_ok or STATE.pending_config_notice or show_tool)
            tool_row.set_visibility(show_tool)
            tool_row.classes(replace="console-notice " + (
                "notice-danger" if operation.get("state") == "failed"
                else "notice-warning" if operation.get("state") == "unknown"
                else "notice-tool"))
            tool_label.set_text(" · ".join(str(v) for v in
                                (operation.get("kind"), operation.get("id"), operation.get("state"), operation.get("error")) if v))
            tool_stop.set_visibility(bool(operation.get("process")) and
                                     operation.get("state") in ("starting", "running", "stopping", "unknown"))
            tool_stop.set_enabled(STATE.env_ok)
            controller_label.set_text("Connected" if STATE.controller_connected else "Disconnected / pending")
            status_label.set_text(f"Source: {STATE.status_source}\n{STATE.status_detail}")
            text, color = _PROC_BADGE[STATE.process_state]
            if STATE.control_uncertain:
                text, color = "State unverified", "warning"
            elif STATE.process_state is ProcState.RUNNING and not STATE.attached:
                text += " (reattached)"
            badge.set_text(text)
            badge.props(f'color="{color}"')
            env_label.set_text(f"{STATE.env_detail or 'Runtime environment unavailable'} — device controls unavailable; offline views remain available")
            env_notice.set_visibility(not STATE.env_ok)
            pending_notice.set_visibility(STATE.pending_config_notice)

        # Stable elements: a timer never recreates a control.
        ui.timer(UI_TICK_S, refresh_header)
        refresh_header()
        yield
