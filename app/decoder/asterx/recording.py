"""Read and parse bounded batches from the AsteRx telemetry CSV files."""

from __future__ import annotations

import os
from dataclasses import dataclass, replace
from pathlib import Path

REPLAY_TAIL_BYTES = 256 * 1024
MAX_ROW_BYTES = 4096

CSV_COLUMNS = {
    "live_insnavgeod.csv": (
        "tow_ms", "host_unix_ns", "gnss_mode", "error", "info", "gnss_age_s",
        "lat_deg", "lon_deg", "height_m", "accuracy_m", "heading_deg", "pitch_deg", "roll_deg"),
    "live_receiverstatus.csv": (
        "tow_ms", "host_unix_ns", "cpu_load_pct", "up_time_s", "rx_status", "rx_error", "ext_error", "temp_c"),
}

def csv_header(name: str, raw: bytes) -> tuple[str, ...]:
    names = tuple(raw.decode("utf-8-sig").strip("\r\n").split(","))
    missing = set(CSV_COLUMNS[name]) - set(names)
    if missing or len(names) != len(set(names)):
        raise ValueError(f"{name}: missing or duplicate columns ({', '.join(sorted(missing))})")
    return names


def csv_fields(raw: bytes, columns: tuple[str, ...]) -> tuple[str, ...] | None:
    try:
        fields = tuple(raw.decode("utf-8").strip("\r\n").split(","))
    except UnicodeError:
        return None
    return fields if len(fields) == len(columns) else None

@dataclass(frozen=True)
class TailCursor:
    offset: int = 0
    columns: tuple[str, ...] = ()
    identity: tuple[int, int] | None = None
    tail_on_open: bool = False
    disabled: bool = False


def read_batch(directory: Path, cursors: dict[str, TailCursor]):
    """Read at most 256 KiB per stream, keeping incomplete rows for the next poll."""
    updated = dict(cursors)
    rows = []
    errors = []
    reset = False
    skipped = 0
    for name, cursor in cursors.items():
        if cursor.disabled:
            continue
        try:
            with (directory / name).open("rb") as stream:
                stat = os.fstat(stream.fileno())
                identity = (stat.st_dev, stat.st_ino)
                if cursor.identity is not None and (identity != cursor.identity or stat.st_size < cursor.offset):
                    cursor = TailCursor(tail_on_open=True)
                    reset = True
                    errors.append(f"{name}: file was replaced or truncated; display was reset")
                if not cursor.columns:
                    header = stream.readline(MAX_ROW_BYTES + 1)
                    if len(header) > MAX_ROW_BYTES:
                        raise ValueError(f"{name}: oversized CSV header")
                    if not header.endswith(b"\n"):
                        continue
                    columns = csv_header(name, header)
                    offset = len(header)
                    if cursor.tail_on_open and stat.st_size - offset > REPLAY_TAIL_BYTES:
                        offset = stat.st_size - REPLAY_TAIL_BYTES
                        stream.seek(offset)
                        partial = stream.readline(MAX_ROW_BYTES + 1)
                        if len(partial) > MAX_ROW_BYTES:
                            raise ValueError(f"{name}: oversized CSV row at the tail seek")
                        offset += len(partial)
                    cursor = TailCursor(offset, columns, identity)
                stream.seek(cursor.offset)
                chunk = stream.read(REPLAY_TAIL_BYTES)
            end = chunk.rfind(b"\n") + 1
            if not end and len(chunk) == REPLAY_TAIL_BYTES:
                raise ValueError(f"{name}: CSV row exceeds the read limit")
            for raw in chunk[:end].splitlines():
                fields = csv_fields(raw, cursor.columns) if len(raw) <= MAX_ROW_BYTES else None
                if fields is None:
                    skipped += 1
                else:
                    rows.append((name, cursor.columns, fields))
            updated[name] = replace(cursor, offset=cursor.offset + end)
        except FileNotFoundError:
            continue  # disabled driver / stream not yet created
        except (ValueError, UnicodeError) as e:
            updated[name] = replace(cursor, disabled=True)
            errors.append(str(e))
        except OSError as e:
            errors.append(f"{name}: {e}")
    return updated, rows, errors, reset, skipped
