"""In-memory preview images served over HTTP instead of inline base64 data URLs.

Inline data URLs travel through the NiceGUI websocket as JSON on every page
open; a URL lets the browser fetch binary bytes once and cache them. Screen
previews are stored as bytes. Full-resolution downloads are registered as
producers that run once, on the first request, in a worker thread; producers
must capture only small values or paths (they may re-read the saved RAW), so
evicting an entry frees its memory. The store is bounded by entry count and
by materialized bytes; the least recently used entries are evicted first.
"""

from __future__ import annotations

import re
import threading
import uuid
from collections import OrderedDict
from collections.abc import Callable
from dataclasses import dataclass, field

ROUTE = "/preview-image"
MAX_ENTRIES = 64
MAX_BYTES = 512 * 1024 * 1024
_SAFE_NAME = re.compile(r"[^A-Za-z0-9._-]+")


@dataclass
class _Entry:
    media_type: str
    filename: str
    data: bytes | None = None
    producer: Callable[[], bytes] | None = None
    lock: threading.Lock = field(default_factory=threading.Lock)


_entries: OrderedDict[str, _Entry] = OrderedDict()
_guard = threading.Lock()


def _materialized_bytes() -> int:
    return sum(len(entry.data) for entry in _entries.values() if entry.data is not None)


def _evict() -> None:
    while len(_entries) > MAX_ENTRIES or (_materialized_bytes() > MAX_BYTES and len(_entries) > 1):
        _entries.popitem(last=False)


def _register(entry: _Entry) -> str:
    key = uuid.uuid4().hex
    with _guard:
        _entries[key] = entry
        _evict()
    return f"{ROUTE}/{key}"


def _filename(name: str) -> str:
    return _SAFE_NAME.sub("_", name).strip("._") or "image.png"


def put(data: bytes, filename: str, media_type: str = "image/png") -> str:
    """Store ready bytes; return the URL path that serves them."""
    if not isinstance(data, bytes) or not data:
        raise ValueError("Preview image data must be nonempty bytes")
    return _register(_Entry(media_type, _filename(filename), data=data))


def lazy(producer: Callable[[], bytes], filename: str, media_type: str = "image/png") -> str:
    """Store a producer that runs on the first request; return its URL path."""
    if not callable(producer):
        raise ValueError("Preview image producer must be callable")
    return _register(_Entry(media_type, _filename(filename), producer=producer))


def download_url(url: str) -> str:
    return f"{url}?download=1"


def release(*urls: str | None) -> None:
    with _guard:
        for url in urls:
            if url and url.startswith(f"{ROUTE}/"):
                _entries.pop(url.removeprefix(f"{ROUTE}/").split("?", 1)[0], None)


def fetch(key: str) -> tuple[bytes, str, str] | None:
    """Bytes, media type and filename; runs a pending producer (blocking)."""
    with _guard:
        entry = _entries.get(key)
        if entry is None:
            return None
        _entries.move_to_end(key)
    with entry.lock:  # concurrent requests run a producer once
        if entry.data is None:
            data = entry.producer()
            if not isinstance(data, bytes) or not data:
                raise ValueError("Preview image producer returned no bytes")
            entry.data, entry.producer = data, None
            with _guard:
                _evict()
    return entry.data, entry.media_type, entry.filename


def clear() -> None:
    with _guard:
        _entries.clear()
