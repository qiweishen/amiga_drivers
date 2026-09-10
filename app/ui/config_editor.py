"""Config editor: raw-text editing (comments survive) with validation + atomic save."""

from __future__ import annotations

from nicegui import ui

from ..constants import CONFIG_FILES
from ..services import config_store
from ..services.config_store import ConflictError
from ..state import STATE, ProcState
from . import layout


_EDIT_ORDER = ("main", "asterx", "fx10", "gox", "lms4xxx")


@ui.page("/config")
def config_page() -> None:
    with layout.frame("Config"):
        ui.label(
            "Edited as raw text (comments are preserved). Validation here is syntax-only; "
            "the C++ loader is the final authority. "
            "Saving is locked during initialization and while acquisition ownership is unknown. "
            "Changes saved while recording take effect on the next start."
        ).classes("text-sm text-gray-600")

        with ui.tabs() as tabs:
            tab_handles = {cid: ui.tab(CONFIG_FILES[cid].label) for cid in _EDIT_ORDER}
        with ui.tab_panels(tabs, value=tab_handles["main"]).classes("w-full"):
            for cid in _EDIT_ORDER:
                with ui.tab_panel(tab_handles[cid]):
                    _editor_panel(cid)


def _editor_panel(config_id: str) -> None:
    try:
        loaded = config_store.read(config_id)
    except Exception as e:
        ui.label(f"Cannot open {config_id} config: {e}").classes("text-red-700")
        ui.label("Check the driver path in Main, save it, then reload this page.")
        ui.link("Reload configuration page", "/config")
        return
    state = {"mtime": loaded.mtime, "saved_text": loaded.text, "path": loaded.file.path}

    path_label = ui.label(str(loaded.file.path)).classes("text-xs text-gray-800 font-mono")
    path_warning = ui.label("").classes("text-sm text-amber-700")
    editor = ui.codemirror(
        value=loaded.text,
        language="YAML",
        theme="basicDark",
    ).classes("w-full h-[60vh] text-s")
    dirty_label = ui.label("").classes("text-xs text-amber-700")

    def _mark_dirty() -> None:
        dirty_label.set_text("Unsaved changes" if editor.value != state["saved_text"] else "")

    editor.on_value_change(lambda _: _mark_dirty())

    def _validate() -> bool:
        errors = config_store.validate(config_id, editor.value)
        if errors:
            for e in errors:
                ui.notify(e, type="negative", multi_line=True)
            return False
        ui.notify("Syntax check passed (the C++ loader has the final say)", type="positive")
        return True

    async def _save(force: bool = False) -> None:
        if config_store.validate(config_id, editor.value):
            with ui.dialog() as dialog, ui.card():
                ui.label("Syntax check failed — save anyway?")
                with ui.row():
                    ui.button("Save anyway", color="negative", on_click=lambda: dialog.submit(True))
                    ui.button("Cancel", on_click=lambda: dialog.submit(False)).props("flat")
            if not await dialog:
                return
        try:
            state["mtime"] = config_store.save(
                config_id, editor.value, None if force else state["mtime"], expected_path=state["path"]
            )
            state["saved_text"] = editor.value
            _mark_dirty()
            if STATE.process_state in (ProcState.RUNNING, ProcState.STARTING):
                STATE.pending_config_notice = True
                ui.notify("Saved — takes effect on the next recording start", type="info")
            else:
                ui.notify("Saved (previous content backed up as .bak)", type="positive")
        except ConflictError as e:
            with ui.dialog() as dialog, ui.card():
                ui.label(f"{e} — how to proceed?")
                with ui.row():
                    ui.button("Reload from disk", on_click=lambda: dialog.submit("reload"))
                    ui.button("Overwrite anyway", color="negative", on_click=lambda: dialog.submit("force"))
                    ui.button("Cancel", on_click=lambda: dialog.submit("cancel")).props("flat")
            choice = await dialog
            if choice == "reload":
                _revert()
            elif choice == "force":
                await _save(force=True)
        except Exception as e:
            ui.notify(f"Config was not saved: {e}", type="negative", multi_line=True)

    def _revert() -> None:
        try:
            fresh = config_store.read(config_id)
        except Exception as e:
            ui.notify(f"Cannot reload config: {e}", type="negative", multi_line=True)
            return
        state["mtime"] = fresh.mtime
        state["saved_text"] = fresh.text
        state["path"] = fresh.file.path
        path_label.set_text(str(fresh.file.path))
        path_warning.set_text("")
        editor.set_value(fresh.text)
        _mark_dirty()

    with ui.row().classes("gap-2"):
        ui.button("Validate", icon="rule", on_click=_validate).props("outline")
        ui.button("Save", icon="save", on_click=lambda: _save())
        ui.button("Revert", icon="undo", on_click=_revert).props("flat")

    def _check_path() -> None:
        try:
            path = config_store.get(config_id).path
            path_warning.set_text("" if path == state["path"] else
                                  f"Main now selects {path}. Revert reloads that file; unsaved text is retained until then.")
        except Exception as e:
            path_warning.set_text(f"Cannot resolve the selected config: {e}")

    ui.timer(2.0, _check_path)
