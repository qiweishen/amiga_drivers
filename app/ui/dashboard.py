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
    with layout.frame("总览"):
        # --- control row -----------------------------------------------------
        with ui.row().classes("items-center gap-4"):
            start_btn = ui.button("启动采集", icon="play_arrow", on_click=_on_start)
            stop_btn = ui.button("停止采集", icon="stop", color="negative", on_click=_confirm_stop)
            session_label = ui.label().classes("text-sm text-gray-600")
        ui.label("提示：关闭 GUI 不会停止采集进程；重新打开会自动接管。").classes("text-xs text-gray-500")

        # --- enable switches -------------------------------------------------
        ui.separator()
        ui.label("传感器启用（写入 config-main.yaml，启动时生效）").classes("font-bold")
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
                    ui.link("查看日志", "/logs").classes("text-sm")

        cards()

        def refresh() -> None:
            running = STATE.process_state in (ProcState.RUNNING, ProcState.STARTING)
            start_btn.set_enabled(STATE.env_ok and not running and STATE.process_state is not ProcState.STOPPING)
            stop_btn.set_enabled(running)
            if STATE.active_session is not None:
                started = STATE.session_started_at or 0
                elapsed = max(0, int(time.time() - started))
                session_label.set_text(
                    f"会话 {STATE.active_session.name} · 已运行 {elapsed // 3600:02d}:{elapsed % 3600 // 60:02d}:{elapsed % 60:02d}"
                    if running
                    else f"上次会话 {STATE.active_session.name}"
                )
            elif STATE.process_state is ProcState.STARTING:
                session_label.set_text("等待会话目录…")
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
            ui.notify("启动中，请稍候再修改传感器启用", type="warning")
            return
        config_store.set_enable(driver, value)
        if STATE.process_state is ProcState.RUNNING:
            STATE.pending_config_notice = True
            ui.notify("已保存，将在下次启动采集时生效", type="info")
    except Exception as e:
        ui.notify(f"写入 Enable 失败: {e}", type="negative")


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
                ui.label("启动前警告").classes("font-bold")
                for w in warnings:
                    ui.label("· " + w).classes("text-sm")
                with ui.row():
                    ui.button("仍然启动", on_click=lambda: dialog.submit(True))
                    ui.button("取消", on_click=lambda: dialog.submit(False)).props("flat")
            if not await dialog:
                return
        await process.start()
        ui.notify("采集已启动", type="positive")
    finally:
        _start_in_flight = False


async def _confirm_stop() -> None:
    with ui.dialog() as dialog, ui.card():
        ui.label("确定停止采集？所有传感器将一起有序关停。")
        with ui.row():
            ui.button("停止", color="negative", on_click=lambda: dialog.submit(True))
            ui.button("取消", on_click=lambda: dialog.submit(False)).props("flat")
    if await dialog:
        await process.stop()
        ui.notify("已发送停止请求", type="info")
