"""Read and parse new rows of the AsteRx telemetry CSV files."""

from __future__ import annotations

import os
from dataclasses import dataclass, replace
from pathlib import Path

REPLAY_TAIL_BYTES = 256 * 1024

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


def _open_cursor(name: str, stream, size: int, cursor: TailCursor, identity) -> TailCursor | None:
    """Parse a stream's header; None while the header line is still incomplete."""
    header = stream.readline()
    if not header.endswith(b"\n"):
        return None
    offset = len(header)
    if cursor.tail_on_open and size - offset > REPLAY_TAIL_BYTES:
        # Replay only the complete rows of the newest REPLAY_TAIL_BYTES.
        stream.seek(size - REPLAY_TAIL_BYTES)
        offset = size - REPLAY_TAIL_BYTES + len(stream.readline())
    return TailCursor(offset, csv_header(name, header), identity)


def _read_stream(path: Path, cursor: TailCursor) -> tuple[TailCursor, list[bytes], bool]:
    """(advanced cursor, complete new rows, whether the file was replaced or truncated)."""
    with path.open("rb") as stream:
        stat = os.fstat(stream.fileno())
        identity = (stat.st_dev, stat.st_ino)
        replaced = cursor.identity is not None and (identity != cursor.identity or stat.st_size < cursor.offset)
        if replaced:
            cursor = TailCursor(tail_on_open=True)
        if not cursor.columns:
            opened = _open_cursor(path.name, stream, stat.st_size, cursor, identity)
            if opened is None:
                return cursor, [], replaced
            cursor = opened
        stream.seek(cursor.offset)
        chunk = stream.read(REPLAY_TAIL_BYTES)
    end = chunk.rfind(b"\n") + 1
    if not end and len(chunk) == REPLAY_TAIL_BYTES:
        raise ValueError(f"{path.name}: CSV row exceeds the read limit")
    return replace(cursor, offset=cursor.offset + end), chunk[:end].splitlines(), replaced


def read_batch(directory: Path, cursors: dict[str, TailCursor]):
    """Read at most 256 KiB per stream, keeping incomplete rows for the next poll."""
    updated, rows, errors = dict(cursors), [], []
    reset, skipped = False, 0
    for name, cursor in cursors.items():
        if cursor.disabled:
            continue
        try:
            cursor, lines, replaced = _read_stream(directory / name, cursor)
        except FileNotFoundError:
            continue  # disabled driver / stream not yet created
        except (ValueError, UnicodeError) as error:
            updated[name] = replace(cursor, disabled=True)
            errors.append(str(error))
            continue
        except OSError as error:
            errors.append(f"{name}: {error}")
            continue
        if replaced:
            reset = True
            errors.append(f"{name}: file was replaced or truncated; display was reset")
        updated[name] = cursor
        for raw in lines:
            fields = csv_fields(raw, cursor.columns)
            if fields is None:
                skipped += 1
            else:
                rows.append((name, cursor.columns, fields))
    return updated, rows, errors, reset, skipped
