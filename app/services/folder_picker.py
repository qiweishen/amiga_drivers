"""Read-only directory browsing inside the GUI server's recording mount.

No desktop subprocess, file contents, directory creation, or configuration write
is involved. Call these filesystem operations in a worker thread from the UI.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from . import config_store


_MAX_DIRECTORIES = 500


@dataclass(frozen=True)
class DirectoryListing:
    root: Path
    path: Path
    parent: Path | None
    directories: tuple[Path, ...]
    truncated: bool = False


def _check_text(raw: str) -> None:
    if not isinstance(raw, str) or not raw:
        raise RuntimeError("A directory path is required")
    if any(ord(ch) < 32 or 127 <= ord(ch) < 160 for ch in raw):
        raise RuntimeError("Directory paths must not contain control characters")


def _resolve(path: Path, *, strict: bool) -> Path:
    try:
        resolved = path.resolve(strict=strict)
    except PermissionError:
        raise RuntimeError(f"Permission denied while resolving directory: {path}") from None
    except (OSError, RuntimeError) as error:
        raise RuntimeError(f"Directory is unavailable or cannot be resolved: {path} ({error})") from None
    _check_text(str(resolved))
    return resolved


def _is_directory(path: Path) -> bool:
    try:
        return path.is_dir()
    except PermissionError:
        raise RuntimeError(f"Permission denied for directory: {path}") from None
    except OSError as error:
        raise RuntimeError(f"Cannot inspect directory: {path} ({error})") from None


def _root_directory() -> Path:
    raw = os.environ.get("AMIGA_DATA_ROOT", "") or str(config_store.REPO_ROOT)
    _check_text(raw)
    root = _resolve(Path(raw), strict=True)
    if not _is_directory(root):
        raise RuntimeError(f"The shared recording directory is unavailable: {root}")
    return root


def _visible_ancestor(path: Path, root: Path) -> Path:
    """Return the ancestor before the first hidden component below root.

    The mount itself and its ancestors may be hidden. Check lexical paths as
    well as resolved targets so a hidden alias cannot bypass this restriction.
    """
    if not path.is_relative_to(root):
        return path
    parent = root
    for part in path.relative_to(root).parts:
        if part.startswith(".") and part not in (".", ".."):
            return parent
        parent = parent / part
    return path


def _checked_directory(raw: str, root: Path) -> Path:
    _check_text(raw)
    path = Path(raw)
    if not path.is_absolute():
        raise RuntimeError("Directory navigation requires an absolute server path")
    if _visible_ancestor(path, root) != path:
        raise RuntimeError("Hidden directories cannot be browsed or selected")
    path = _resolve(path, strict=True)
    if not path.is_relative_to(root):
        raise RuntimeError(f"Directory must remain inside the shared recording mount {root}")
    if _visible_ancestor(path, root) != path:
        raise RuntimeError("Hidden directories cannot be browsed or selected")
    if not _is_directory(path):
        raise RuntimeError(f"The server directory is no longer available: {path}")
    return path


def initial_directory(raw: str) -> Path:
    """Open the existing visible parent of an output draft, or the mount root."""
    root = _root_directory()
    if not raw:
        return root
    _check_text(raw)
    if not raw.strip():
        return root
    path = Path(raw)
    if not path.is_absolute():
        path = config_store.REPO_ROOT / path
    path = _resolve(_visible_ancestor(path, root), strict=False)
    if not path.is_relative_to(root):
        return root
    path = _visible_ancestor(path, root)
    while not _is_directory(path):
        if path == root:
            raise RuntimeError(f"The shared recording directory is unavailable: {root}")
        path = path.parent
    return path


def list_directory(raw: str) -> DirectoryListing:
    """List one level of visible directories, with at most 500 returned entries."""
    root = _root_directory()
    path = _checked_directory(raw, root)
    directories: list[Path] = []
    seen: set[Path] = set()
    truncated = False
    try:
        with os.scandir(path) as entries:
            for entry in entries:
                if entry.name.startswith("."):
                    continue
                try:
                    _check_text(entry.name)
                    if not entry.is_dir(follow_symlinks=True):
                        continue
                    child = _checked_directory(str(path / entry.name), root)
                except (OSError, RuntimeError):
                    # A removed, inaccessible, looping, hidden-target or
                    # escaping link must not prevent browsing other folders.
                    continue
                if child in seen:
                    continue
                if len(directories) == _MAX_DIRECTORIES:
                    truncated = True
                    break
                directories.append(child)
                seen.add(child)
    except PermissionError:
        raise RuntimeError(f"Permission denied while listing directory: {path}") from None
    except OSError as error:
        raise RuntimeError(f"Cannot list server directory: {path} ({error})") from None
    # Detect replacement/removal while the listing was being assembled. The
    # next navigation and final selection independently validate again.
    if _checked_directory(str(path), root) != path:
        raise RuntimeError("The server directory changed while it was being listed; open it again")
    directories.sort(key=lambda child: (child.name.casefold(), child.name, str(child)))
    return DirectoryListing(root, path, None if path == root else path.parent,
                            tuple(directories), truncated)


def checked_selection(raw: str) -> str:
    """Return a canonical, existing server directory without saving the draft."""
    root = _root_directory()
    path = _checked_directory(raw, root)
    try:
        # Opening the directory checks listing permission without reading any
        # contents. Access may have changed since the dialog displayed it.
        with os.scandir(path):
            pass
    except PermissionError:
        raise RuntimeError(f"Permission denied for selected directory: {path}") from None
    except OSError as error:
        raise RuntimeError(f"The selected server directory is unavailable: {path} ({error})") from None
    if _checked_directory(str(path), root) != path:
        raise RuntimeError("The selected directory changed; select it again")
    problems = config_store.output_dir_problems(str(path))
    if problems:
        raise RuntimeError("; ".join(problems))
    return str(path)
