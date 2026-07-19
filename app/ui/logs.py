"""Live log page: unified session log with level/module filters + follow mode."""

from __future__ import annotations

from nicegui import ui

from ..services.log_buffer import BUFFER, LEVELS, LogLine
from . import layout

_DEFAULT_LEVELS = {"info", "warning", "error", "critical"}


@ui.page("/logs")
def logs_page() -> None:
    with layout.frame("日志"):
        with ui.row().classes("items-center gap-4"):
            level_select = ui.select(
                list(LEVELS), value=sorted(_DEFAULT_LEVELS), multiple=True, label="级别"
            ).classes("w-64").props("use-chips dense")
            module_select = ui.select(
                sorted(BUFFER.modules_seen) or [], value=[], multiple=True, label="模块（空=全部）"
            ).classes("w-64").props("use-chips dense")
            follow = ui.switch("跟随", value=True)
            ui.button("清屏", on_click=lambda: log.clear()).props("flat dense")
        log = ui.log(max_lines=1000).classes("w-full h-[70vh] font-mono text-xs")

        def _filters() -> tuple[set[str], set[str]]:
            levels = set(level_select.value or [])
            modules = set(module_select.value or [])
            return levels, modules

        def _reload() -> None:
            log.clear()
            levels, modules = _filters()
            for line in BUFFER.snapshot(levels or None, modules or None)[-1000:]:
                log.push(line.raw)
            # refresh module options as new modules appear
            module_select.set_options(sorted(BUFFER.modules_seen), value=list(module_select.value or []))

        def _on_line(line: LogLine) -> None:
            if not follow.value:
                return
            levels, modules = _filters()
            if BUFFER.matches(line, levels or None, modules or None):
                log.push(line.raw)

        unsubscribe = BUFFER.subscribe(_on_line)
        # on_delete (not on_disconnect): transient socket drops must keep the
        # subscription alive — the outbox replays pushes on reconnect.
        ui.context.client.on_delete(lambda: unsubscribe())

        level_select.on_value_change(lambda _: _reload())
        module_select.on_value_change(lambda _: _reload())
        follow.on_value_change(lambda e: _reload() if e.value else None)
        _reload()
