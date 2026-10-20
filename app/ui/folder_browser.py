"""Per-page directory chooser for the recording server's filesystem."""

from __future__ import annotations

import asyncio

from nicegui import ui

from ..services import folder_picker


def _initial_listing(raw: str) -> folder_picker.DirectoryListing:
    return folder_picker.list_directory(str(folder_picker.initial_directory(raw)))


class _FolderBrowser:
    def __init__(self, initial: str) -> None:
        self._initial = initial
        self._alive = True
        self._busy = False
        self._listing: folder_picker.DirectoryListing | None = None
        self._folder_buttons: list[ui.button] = []

        with ui.dialog() as self.dialog, ui.card().classes("folder-browser"):
            with ui.row().classes("section-heading"):
                ui.label("Select output directory").classes("section-title")
                ui.label("Recording computer").classes("section-copy")
            ui.label(
                "Browse directories available to the recording service. "
                "Your selection is saved only when you click Save in Next recording."
            ).classes("section-copy")
            self._root_label = ui.label().classes("section-copy mono")
            with ui.row().classes("folder-navigation"):
                self._root_button = ui.button("Root", icon="home", on_click=self._root).props("outline dense")
                self._up_button = ui.button("Up", icon="arrow_upward", on_click=self._up).props("outline dense")
                self._refresh_button = ui.button("Refresh", icon="refresh", on_click=self._refresh).props("flat dense")
            with ui.row().classes("folder-path-row"):
                self._path = ui.input("Directory", value=initial).props(
                    "outlined dense"
                ).classes("folder-path")
                self._path.on("keydown.enter", self._open_path)
                self._open_button = ui.button("Open", on_click=self._open_path).props("outline dense")
            self._error = ui.label().classes("folder-error")
            self._status = ui.label("Loading directories…").classes("section-copy")
            self._folders = ui.column().classes("folder-list")
            with ui.row().classes("folder-actions"):
                ui.button("Cancel", on_click=lambda: self.dialog.submit(None)).props("flat dense")
                self._select_button = ui.button("Use this folder", icon="check", on_click=self._select).props(
                    "unelevated dense"
                )
        self._path.on_value_change(lambda _: self._controls())
        self._error.set_visibility(False)
        self._controls()

    def _controls(self) -> None:
        if not self._alive:
            return
        listing = self._listing
        available = not self._busy
        self._path.set_enabled(available)
        self._open_button.set_enabled(available and bool((self._path.value or "").strip()))
        self._root_button.set_enabled(available and listing is not None and listing.path != listing.root)
        self._up_button.set_enabled(available and listing is not None and listing.parent is not None)
        self._refresh_button.set_enabled(available)
        self._select_button.set_enabled(
            available and listing is not None and self._path.value == str(listing.path)
        )
        for button in self._folder_buttons:
            button.set_enabled(available)

    async def _load(self, raw: str, *, initial: bool = False) -> None:
        if not self._alive or self._busy:
            return
        self._busy = True
        self._error.set_visibility(False)
        self._status.set_text("Loading directories…")
        self._controls()
        try:
            listing = await asyncio.to_thread(_initial_listing if initial else folder_picker.list_directory, raw)
            if not self._alive:
                return
            self._listing = listing
            self._path.set_value(str(listing.path))
            self._root_label.set_text(f"Available root: {listing.root}")
            self._folders.clear()
            self._folder_buttons.clear()
            with self._folders:
                for directory in listing.directories:
                    button = ui.button(
                        directory.name, icon="folder_open",
                        on_click=lambda _, path=directory: self._load(str(path)),
                    ).props("flat no-caps align=left").classes("folder-entry")
                    self._folder_buttons.append(button)
                if not listing.directories:
                    ui.label("No subfolders. You can select the current directory.").classes("folder-empty")
            count = len(listing.directories)
            self._status.set_text(
                f"Showing {count} folders; more are available. Enter a directory above to open it."
                if listing.truncated else f"{count} folder{'s' if count != 1 else ''} · Hidden folders are excluded"
            )
        except (OSError, RuntimeError, ValueError) as exc:
            if self._alive:
                self._error.set_text(str(exc))
                self._error.set_visibility(True)
                self._status.set_text("Directory unavailable. Enter another path or refresh to try again.")
        finally:
            self._busy = False
            self._controls()

    async def _open_path(self) -> None:
        await self._load(self._path.value or "")

    async def _root(self) -> None:
        if self._listing:
            await self._load(str(self._listing.root))

    async def _up(self) -> None:
        if self._listing and self._listing.parent:
            await self._load(str(self._listing.parent))

    async def _refresh(self) -> None:
        if self._listing:
            await self._load(str(self._listing.path))
        else:
            await self._load(self._initial, initial=True)

    async def _select(self) -> None:
        listing = self._listing
        if not self._alive or self._busy or listing is None or self._path.value != str(listing.path):
            return
        self._busy = True
        self._controls()
        try:
            selected = await asyncio.to_thread(folder_picker.checked_selection, str(listing.path))
            if self._alive:
                self.dialog.submit(selected)
        except (OSError, RuntimeError, ValueError) as exc:
            if self._alive:
                self._error.set_text(str(exc))
                self._error.set_visibility(True)
        finally:
            self._busy = False
            self._controls()


async def select_directory(initial: str) -> str | None:
    """Browse existing server directories without changing configuration."""
    browser = _FolderBrowser(initial)
    loading = asyncio.create_task(browser._load(initial, initial=True))
    try:
        return await browser.dialog
    finally:
        browser._alive = False
        loading.cancel()
        try:
            await loading
        except asyncio.CancelledError:
            pass
        if not browser.dialog.is_deleted:
            browser.dialog.delete()
