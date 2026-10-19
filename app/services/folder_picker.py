"""Select a directory on the GUI server's desktop without changing config."""

from __future__ import annotations

import asyncio
import os
import shutil
from pathlib import Path

from . import config_store


_active = False
_TIMEOUT_S = 300


def _initial_directory(raw: str) -> Path:
    path = config_store.resolve_output_dir(raw).resolve()
    root_value = os.environ.get("AMIGA_DATA_ROOT", "")
    root = Path(root_value).resolve() if root_value else Path(path.anchor)
    if not root.is_dir():
        raise RuntimeError(f"The shared recording directory is unavailable: {root}")
    if not path.is_relative_to(root):
        path = root
    while not path.is_dir():
        if path == root:
            raise RuntimeError(f"No existing directory is available inside {root}")
        path = path.parent
    return path


def _checked_selection(raw: str) -> str:
    if not raw or any(ord(ch) < 32 or 127 <= ord(ch) < 160 for ch in raw):
        raise RuntimeError("The selected directory contains unsupported control characters")
    path = Path(raw)
    if not path.is_absolute():
        raise RuntimeError("The folder picker did not return an absolute directory path")
    path = path.resolve()
    if any(ord(ch) < 32 or 127 <= ord(ch) < 160 for ch in str(path)):
        raise RuntimeError("The selected directory resolves to a path with unsupported control characters")
    if not path.is_dir():
        raise RuntimeError("The selected directory is no longer available on the GUI server")
    problems = config_store.output_dir_problems(str(path))
    if problems:
        raise RuntimeError("; ".join(problems))
    return str(path)


async def select_directory(initial: str) -> str | None:
    """Return a server-side absolute path, or None when the user cancels.

    The server/container must already have zenity and access to the user's
    X11 desktop. Browser-local directory handles are not acquisition paths.
    """
    global _active
    if _active:
        raise RuntimeError("A folder dialog is already open on the GUI server's desktop")
    if not os.environ.get("DISPLAY"):
        raise RuntimeError(
            "Native folder selection requires desktop access (DISPLAY) on the GUI server/container. "
            "Enter the output directory manually."
        )
    executable = shutil.which("zenity")
    if executable is None:
        raise RuntimeError(
            "Native folder selection is unavailable: zenity is not installed in the GUI server/container. "
            "Enter the output directory manually."
        )
    _active = True
    process = None
    spawn = None
    try:
        directory = await asyncio.to_thread(_initial_directory, initial)
        spawn = asyncio.create_task(asyncio.create_subprocess_exec(
            executable, "--file-selection", "--directory", "--title=Select Output Directory",
            "--filename=" + str(directory).rstrip(os.sep) + os.sep,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        ))
        # A cancellation during process creation must still reap the child.
        try:
            process = await asyncio.shield(spawn)
        except OSError:
            spawn = None  # Creation failed; there is no process to clean up.
            raise RuntimeError("The system folder dialog could not start; check the zenity installation") from None
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(), _TIMEOUT_S)
        except asyncio.TimeoutError:
            raise RuntimeError("Folder selection timed out after five minutes; open the dialog again") from None
        if process.returncode == 1:
            return None
        if process.returncode != 0:
            detail = stderr.decode("utf-8", errors="replace").strip()[:300]
            raise RuntimeError(
                "The system folder dialog could not open. Check the GUI server's desktop access."
                + (f" {detail}" if detail else "")
            )
        try:
            selected = stdout.decode("utf-8").removesuffix("\n")
        except UnicodeDecodeError:
            raise RuntimeError("The folder picker returned an unreadable directory path") from None
        return await asyncio.to_thread(_checked_selection, selected)
    finally:
        try:
            if process is None and spawn is not None:
                process = await spawn
            if process is not None and process.returncode is None:
                try:
                    process.kill()
                except ProcessLookupError:
                    pass
                await process.communicate()
        finally:
            _active = False
