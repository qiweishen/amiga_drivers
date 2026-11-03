"""Offline inspection/extraction for eBUS raw; legacy files remain readable."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace

from . import recording


def targets(path):
    path = Path(path)
    if path.is_file():
        return [path]
    if path.is_dir():
        return sorted(path.rglob("seg_*.raw"))
    raise ValueError(f"No such recording: {path}")


def contains_new_format(path):
    return any(recording.is_segment(segment) for segment in targets(path))


def _publish(part, destination):
    os.link(part, destination)  # fails on collision; never replaces an earlier result
    part.unlink()
    descriptor = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _rebuild(segment, destination):
    """Recover complete metadata records before a torn tail; retain raw untouched."""
    part = Path(str(destination) + ".part") if destination else None
    if destination and destination.exists():
        raise FileExistsError(f"Refusing to overwrite {destination}")
    out = part.open("x", encoding="utf-8") if part else sys.stdout
    count = 0
    try:
        for record in recording.iter_records(segment, live=True):
            row = {"format": "ebus-raw-index-v1", "off": record.offset, "seq": record.sequence,
                   "psz": record.payload_size, "metadata": record.metadata}
            out.write(json.dumps(row, separators=(",", ":"), allow_nan=False) + "\n")
            count += 1
        if part:
            out.flush()
            os.fsync(out.fileno())
    finally:
        if part:
            out.close()
    if part:
        _publish(part, destination)
    print(f"rebuild-index: {segment}: {count} complete records; raw unchanged; payload CRC not audited", file=sys.stderr)


def dispatch(args):
    from gox_driver.scripts import inspect_raw as legacy
    path = Path(getattr(args, "path", getattr(args, "segment", "")))
    segments = targets(path)
    if not segments:
        raise ValueError("No seg_*.raw files found")
    if args.cmd == "verify":
        errors = 0
        for segment in segments:
            try:
                if not recording.is_segment(segment):
                    errors += legacy.cmd_verify(SimpleNamespace(path=str(segment), index=getattr(args, "index", None)))
                    continue
                if getattr(args, "index", None):
                    raise ValueError("--index is a legacy jai-raw-seg option; eBUS records contain their own metadata")
                count = valid = 0
                for record in recording.iter_records(segment):
                    recording.verify_payload(record)
                    count += 1
                    valid += record.valid_frame
                print(f"verify: {segment}: {count} records, {valid} valid frames; CRCs verified")
            except (OSError, ValueError) as error:
                print(f"error: {segment}: {error}", file=sys.stderr)
                errors += 1
        if path.is_dir():
            for directory in sorted({segment.parent for segment in segments if recording.is_segment(segment)}):
                try:
                    for _ in recording.session_records(directory):
                        pass
                except (OSError, ValueError) as error:
                    print(f"error: {directory}: {error}", file=sys.stderr)
                    errors += 1
        return int(errors != 0)
    if args.cmd == "rebuild-index":
        if path.is_dir() and args.output:
            raise ValueError("--output requires a single segment")
        for segment in segments:
            if not recording.is_segment(segment):
                legacy.cmd_rebuild_index(SimpleNamespace(path=str(segment), output=args.output if path.is_file() else str(segment.with_suffix(".rebuilt.idx.jsonl"))))
                continue
            output = (Path(args.output) if args.output else None) if path.is_file() else segment.with_suffix(".rebuilt.idx.jsonl")
            _rebuild(segment, output)
        return 0
    if len(segments) != 1:
        raise ValueError("list/extract require one segment")
    segment = segments[0]
    if not recording.is_segment(segment):
        return legacy.cmd_list(args) if args.cmd == "list" else legacy.cmd_extract(args)
    count = 0
    for record in recording.iter_records(segment):
        if args.cmd == "list":
            print(f"seq={record.sequence} bid={record.bid} valid={record.valid_frame} "
                  f"pf=0x{record.pf:08x} {record.w}x{record.h} bytes={record.payload_size} off={record.offset}")
            count += 1
            if args.limit and count >= args.limit:
                break
        elif record.sequence == args.seq:
            recording.verify_payload(record)
            with segment.open("rb") as source, Path(args.output).open("xb") as destination:
                source.seek(record.payload_offset)
                remaining = record.payload_size
                while remaining:
                    chunk = source.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError("Payload became incomplete during extraction")
                    destination.write(chunk)
                    remaining -= len(chunk)
            return 0
    if args.cmd == "extract":
        raise ValueError("Requested record sequence was not found")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Inspect eBUS raw and legacy GoX recordings without an SDK")
    commands = parser.add_subparsers(dest="cmd", required=True)
    listing = commands.add_parser("list")
    listing.add_argument("segment")
    listing.add_argument("--limit", type=int, default=0)
    verify = commands.add_parser("verify")
    verify.add_argument("path")
    verify.add_argument("--index", help="Legacy jai-raw-seg index only")
    rebuild = commands.add_parser("rebuild-index")
    rebuild.add_argument("path")
    rebuild.add_argument("-o", "--output")
    extract = commands.add_parser("extract")
    extract.add_argument("segment")
    extract.add_argument("--seq", type=int, required=True)
    extract.add_argument("-o", "--output", required=True)
    args = parser.parse_args(argv)
    try:
        return dispatch(args)
    except (OSError, ValueError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
