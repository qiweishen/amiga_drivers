"""Where unconfirmed references live: <Output Directory>/tmp until the operator saves them."""

from __future__ import annotations

import os
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


def move_to_output(session: Path, root: Path) -> Path:
    """Rename ``session`` from ``root``/tmp to ``root``; refuses anything else or an existing target."""
    if session.parent != root / TMP_FOLDER:
        raise ValueError(f"Only references under {root / TMP_FOLDER} can be saved")
    target = root / session.name
    if target.exists():
        raise ValueError(f"{target} already exists")
    os.rename(session, target)
    return target
