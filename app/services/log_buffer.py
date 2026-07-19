"""Ring buffer of parsed log lines + live fan-out to page subscribers."""

from __future__ import annotations

import re
from collections import deque
from dataclasses import dataclass
from typing import Callable

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# File-log pattern: "[HH:MM:SS] [level] [Module]: msg" (date lives in the
# session directory name).
LINE_RE = re.compile(r"^\[(\d{2}:\d{2}:\d{2})\] \[(\w+)\] \[([^\]]+)\]: (.*)$")

LEVELS = ("trace", "debug", "info", "warning", "error", "critical")


@dataclass
class LogLine:
    ts: str  # "HH:MM:SS" or ""
    level: str  # spdlog level token, or "" when unparsed
    module: str  # e.g. "Main", "GoXApp", or "stderr" for pre-session pipe lines
    msg: str
    raw: str


def parse_line(raw: str, *, fallback_module: str = "") -> LogLine:
    raw = ANSI_RE.sub("", raw.rstrip("\n"))
    m = LINE_RE.match(raw)
    if m:
        return LogLine(m.group(1), m.group(2), m.group(3), m.group(4), raw)
    return LogLine("", "", fallback_module, raw, raw)


class LogBuffer:
    def __init__(self, maxlen: int = 5000) -> None:
        self._lines: deque[LogLine] = deque(maxlen=maxlen)
        self._subscribers: list[Callable[[LogLine], None]] = []
        self.modules_seen: set[str] = set()

    def clear(self) -> None:
        self._lines.clear()
        self.modules_seen.clear()

    def append(self, line: LogLine) -> None:
        self._lines.append(line)
        if line.module:
            self.modules_seen.add(line.module)
        for cb in list(self._subscribers):
            try:
                cb(line)
            except Exception:
                pass  # a broken page subscriber must never break the feed

    def subscribe(self, cb: Callable[[LogLine], None]) -> Callable[[], None]:
        self._subscribers.append(cb)

        def unsubscribe() -> None:
            if cb in self._subscribers:
                self._subscribers.remove(cb)

        return unsubscribe

    @staticmethod
    def matches(line: LogLine, levels: set[str] | None, modules: set[str] | None) -> bool:
        if levels and line.level and line.level not in levels:
            return False
        if modules and line.module and line.module not in modules:
            return False
        return True

    def snapshot(self, levels: set[str] | None = None, modules: set[str] | None = None) -> list[LogLine]:
        return [ln for ln in self._lines if self.matches(ln, levels, modules)]

    def last_error_line(self) -> LogLine | None:
        for ln in reversed(self._lines):
            if ln.level in ("error", "critical") or (
                ln.module == "Main" and ("failed" in ln.msg or "exception" in ln.msg)
            ):
                return ln
        return None


BUFFER = LogBuffer()
