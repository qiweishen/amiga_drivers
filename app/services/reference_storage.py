"""Where unconfirmed references live: <Output Directory>/tmp until the operator saves them."""

from __future__ import annotations

import os
from collections.abc import Callable
from datetime import datetime, timezone
from pathlib import Path

from . import config_store

TMP_FOLDER = "tmp"


def output_root() -> Path:
    settings = config_store.main_settings()
    problems = config_store.output_dir_problems(settings["output_dir_raw"])
    if problems:
        raise ValueError("; ".join(problems))
    return settings["output_dir"].resolve()


def tmp_root() -> Path:
    """Unconfirmed references and exposure series live in <output>/tmp until saved."""
    return output_root() / TMP_FOLDER


def save(session: Path, record: Callable[[Path, dict], None]) -> Path:
    """Move ``session`` from <output>/tmp into <output>; never replaces an existing directory.

    ``record(session, note)`` writes the move note (saved_utc, saved_from) into the
    session before it is renamed.
    """
    root = output_root()
    target = root / session.name
    if session.parent != root / TMP_FOLDER:
        raise ValueError(f"Only references under {root / TMP_FOLDER} can be saved")
    if target.exists():
        raise ValueError(f"{target} already exists")
    record(session, {"saved_utc": datetime.now(timezone.utc).isoformat(),
                     "saved_from": session.relative_to(root).as_posix()})
    os.rename(session, target)
    return target
