"""One control authority per execution environment, held for the server lifetime."""

from __future__ import annotations

import fcntl
import os
from pathlib import Path

from ..constants import RUNTIME_DIR

_file = None


def acquire() -> None:
    global _file
    if _file is not None:
        return
    # The host GUI and the in-container controller see the same bind-mounted
    # inode. Backend-specific /tmp locks would allow both to claim the devices.
    path = Path(os.environ.get("AMIGA_CONTROL_LOCK", str(RUNTIME_DIR / "controller.lock")))
    handle = path.open("a+")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BaseException:
        handle.close()
        raise RuntimeError(f"Another control server owns {path}; use its GUI/API instead") from None
    _file = handle  # Never unlink a lock file: another process may already have it open.


def require() -> None:
    if _file is None:
        raise RuntimeError("This process does not own acquisition control")


def release() -> None:
    global _file
    if _file is not None:
        _file.close()
        _file = None
