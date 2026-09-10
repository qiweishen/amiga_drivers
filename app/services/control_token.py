"""Controller credential loading and opt-in creation for the shared dev stack."""

from __future__ import annotations

import os
import secrets
import tempfile
from pathlib import Path


def _create_if_missing(path: Path) -> None:
    try:
        path.lstat()
        return  # Never rotate or repair an existing credential implicitly.
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    # mkstemp creates mode 0600. Publish a fully flushed file without replacing
    # one another process may have created; a client never sees a partial token.
    fd, name = tempfile.mkstemp(prefix=".controller-token-", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            stream.write(secrets.token_hex(32) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        try:
            os.link(temporary, path)
        except FileExistsError:
            pass
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def read(*, create: bool = False) -> str:
    filename = os.environ.get("AMIGA_CONTROLLER_TOKEN_FILE", "")
    if create:
        if not filename:
            raise ValueError("Token bootstrap requires AMIGA_CONTROLLER_TOKEN_FILE")
        _create_if_missing(Path(filename))
    if filename:
        with Path(filename).open(encoding="utf-8") as stream:
            value = stream.read(4098)
            if stream.read(1):
                raise ValueError("Controller token file exceeds its size limit")
        value = value.strip()
    else:
        value = os.environ.get("AMIGA_CONTROLLER_TOKEN", "").strip()
    if not 32 <= len(value) <= 4096 or not value.isascii() or any(ord(c) <= 32 or ord(c) >= 127 for c in value):
        raise ValueError("Configure a controller token of 32–4096 printable ASCII characters without spaces")
    return value
