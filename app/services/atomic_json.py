"""Small controller journals: fsync content, atomically replace, sync directory."""

from __future__ import annotations

import json
import os
from pathlib import Path


def write(path: Path, value, *, limit: int) -> None:
    raw = json.dumps(value, allow_nan=False).encode("utf-8")
    if len(raw) > limit:
        raise ValueError("Controller journal exceeds its size limit")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    with temporary.open("wb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
