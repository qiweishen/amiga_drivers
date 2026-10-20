"""Explicitly saved next-recording metadata with draft and conflict protection."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from dataclasses import dataclass

from nicegui import ui

from ..services import config_actions, config_store
from . import folder_browser


_FIELDS = (("Operator", "operator"), ("Field", "field"),
           ("Output Directory", "output_dir_raw"))


@dataclass
class _Draft:
    baseline: str | None = None
    value: str = ""
    dirty: bool = False
    saving: bool = False
    error: str = ""
    conflict: bool = False


class RecordingSettings:
    """One browser page's drafts, written only by the Save button."""

    def __init__(self, on_pending: Callable[[], None]) -> None:
        self._alive = True
        self._enabled = False
        self._syncing = False
        self._read_error = ""
        self._folder_error = ""
        self._selecting_folder = False
        self._folder_task: asyncio.Task | None = None
        self._on_pending = on_pending
        self._drafts = {key: _Draft() for key, _ in _FIELDS}
        self._inputs: dict[str, ui.input] = {}
        self._hints: dict[str, ui.label] = {}

        with ui.column().classes("recording-settings w-full gap-2"):
            with ui.element("div").classes("metadata-grid"):
                for key, _ in _FIELDS:
                    with ui.column().classes(
                        "metadata-field w-full min-w-0 gap-1"
                        + (" metadata-output" if key == "Output Directory" else "")
                    ):
                        with ui.row().classes("metadata-input-row w-full items-center gap-2 flex-nowrap"):
                            field = ui.input(key).props("outlined dense").classes("flex-1 min-w-0")
                            if key == "Output Directory":
                                self._choose_folder = ui.button(
                                    "Select Folder", icon="folder_open", on_click=self.choose_folder,
                                ).props("outline dense no-wrap").classes("metadata-folder")
                                self._choose_folder.tooltip("Browse the recording computer's directories in this page")
                        field.disable()
                        field.on_value_change(lambda e, k=key: self._edit(k, e.value))
                        self._inputs[key] = field
                        self._hints[key] = ui.label().classes("metadata-hint")
            ui.label("Paths refer to the GUI server/container. Relative paths use the project root.").classes("metadata-help")
            with ui.row().classes("metadata-actions"):
                self._status = ui.label("Loading recording details…").classes("metadata-status")
                self._save = ui.button("Save", icon="save", on_click=self.save).props("unelevated dense")
                self._reload = ui.button("Reload", icon="refresh", on_click=self.reload).props("flat dense")
            self._notice = ui.label().classes("metadata-hint")
        self._render()
        ui.context.client.on_delete(self._dispose)

    @property
    def ready(self) -> bool:
        return (self._alive and not self._read_error and not self._selecting_folder
                and all(d.baseline is not None and not (d.dirty or d.saving or d.conflict)
                        for d in self._drafts.values()))

    def _dispose(self) -> None:
        self._alive = False
        if self._folder_task is not None:
            self._folder_task.cancel()

    def set_enabled(self, enabled: bool) -> None:
        if self._enabled == enabled:
            return
        self._enabled = enabled
        self._render()

    def read_failed(self, error: Exception) -> None:
        if not self._alive:
            return
        self._read_error = str(error)
        self._on_pending()
        self._render()

    def sync(self, settings: dict, *, discard: bool = False) -> None:
        """Load a snapshot on entry, after Save, or after an explicit reload."""
        if not self._alive:
            return
        self._read_error = ""
        self._folder_error = ""
        self._syncing = True
        try:
            for key, setting in _FIELDS:
                draft = self._drafts[key]
                if draft.saving:
                    continue
                value = settings[setting]
                if discard or draft.baseline is None or not (draft.dirty or draft.conflict):
                    draft.baseline = draft.value = value
                    draft.dirty = draft.conflict = False
                    draft.error = ""
                    if self._inputs[key].value != value:
                        self._inputs[key].set_value(value)
                elif draft.value == value:
                    # Another client may already have saved exactly this value.
                    draft.baseline = value
                    draft.dirty = draft.conflict = False
                    draft.error = ""
                elif value != draft.baseline:
                    draft.conflict = True
                    draft.error = ("Changed in the configuration file. Your edit is retained; "
                                   "discard edits and reload to use the file's values.")
        finally:
            self._syncing = False
        if not self.ready:
            self._on_pending()
        self._render()

    def _edit(self, key: str, value: str | None) -> None:
        if self._syncing or not self._alive:
            return
        draft = self._drafts[key]
        if key == "Output Directory":
            self._folder_error = ""
        draft.value = value or ""
        draft.dirty = draft.value != draft.baseline
        if not draft.conflict:
            draft.error = ""
        if draft.dirty:
            self._on_pending()
        self._render()

    async def choose_folder(self) -> None:
        if (not self._alive or not self._enabled or self._read_error or self._selecting_folder
                or any(d.saving for d in self._drafts.values())
                or self._drafts["Output Directory"].baseline is None):
            return
        initial = self._drafts["Output Directory"].value
        self._selecting_folder = True
        self._folder_task = asyncio.current_task()
        self._folder_error = ""
        self._on_pending()
        self._render()
        try:
            selected = await folder_browser.select_directory(initial)
            if selected is not None and self._alive:
                if not self._enabled or self._drafts["Output Directory"].value != initial:
                    self._folder_error = "The recording settings changed while the dialog was open. Select the folder again."
                else:
                    # This is an ordinary draft edit; Save remains the only
                    # action that writes recording metadata to the YAML file.
                    self._inputs["Output Directory"].set_value(selected)
        except Exception as exc:
            self._folder_error = str(exc)
        finally:
            self._selecting_folder = False
            self._folder_task = None
            self._render()

    def _read_current(self, *, discard: bool = False) -> None:
        if not self._alive:
            return
        try:
            settings = config_store.main_settings()
        except Exception as exc:
            self.read_failed(exc)
            return
        self.sync(settings, discard=discard)

    def reload(self) -> None:
        # The button explicitly identifies when this discards local edits.
        if not self._selecting_folder and not any(d.saving for d in self._drafts.values()):
            self._read_current(discard=True)

    async def save(self) -> None:
        if (not self._alive or not self._enabled or self._read_error or self._selecting_folder
                or any(d.saving or d.conflict or d.baseline is None for d in self._drafts.values())):
            return
        changed = {key: draft for key, draft in self._drafts.items() if draft.dirty}
        if not changed:
            return
        submitted = {key: draft.value for key, draft in changed.items()}
        expected = {key: draft.baseline for key, draft in changed.items()}
        for draft in changed.values():
            draft.saving = True
            draft.error = ""
        self._on_pending()
        self._render()
        saved = False
        try:
            await config_actions.change("set_main_fields", submitted, expected_values=expected)
        except config_store.ConflictError as exc:
            for draft in changed.values():
                draft.conflict = True
                draft.error = str(exc)
        except Exception as exc:
            for draft in changed.values():
                draft.error = str(exc)
        else:
            saved = True
            for key, draft in changed.items():
                draft.baseline = submitted[key]
                draft.dirty = draft.value != submitted[key]
                draft.error = ""
        finally:
            for draft in changed.values():
                draft.saving = False
            if self._alive:
                if saved:
                    self._read_current()
                else:
                    self._render()

    def _render(self) -> None:
        if not self._alive:
            return
        drafts = tuple(self._drafts.values())
        saving = any(d.saving for d in drafts)
        pending = any(d.dirty or d.conflict for d in drafts)
        failed = bool(self._read_error) or any(d.error for d in drafts)
        if self._selecting_folder:
            text = "Select a folder in the directory browser…"
        elif saving:
            text = "Saving recording details…"
        elif pending:
            text = "Unsaved changes"
        elif failed:
            text = "Recording details need attention before starting."
        elif self.ready:
            text = "Saved"
        else:
            text = "Loading recording details…"
        self._status.set_text(text)
        self._status.classes(add="text-amber-700" if failed or pending else "muted",
                             remove="muted" if failed or pending else "text-amber-700")
        for key, draft in self._drafts.items():
            self._inputs[key].set_enabled(self._enabled and draft.baseline is not None and not self._read_error
                                          and not (key == "Output Directory" and self._selecting_folder))
            hint = draft.error or ("Saving…" if draft.saving else "")
            self._hints[key].set_text(hint)
            self._hints[key].set_visibility(bool(hint))
        self._save.set_visibility(pending or saving)
        self._save.set_enabled(self._enabled and not saving and not self._read_error and not self._selecting_folder
                               and all(d.baseline is not None and not d.conflict for d in drafts))
        self._choose_folder.set_enabled(self._enabled and not saving and not self._read_error
                                        and not self._selecting_folder
                                        and self._drafts["Output Directory"].baseline is not None)
        self._reload.set_text("Discard edits & reload" if pending else "Reload")
        self._reload.set_enabled(not saving and not self._selecting_folder)
        notice = (f"Configuration unavailable: {self._read_error}" if self._read_error
                  else self._folder_error or ("Configuration is temporarily locked."
                                             if pending and not self._enabled else ""))
        self._notice.set_text(notice)
        self._notice.set_visibility(bool(notice))
