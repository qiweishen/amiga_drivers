#!/usr/bin/env python3
"""Inspect, verify and repair "jai-raw-seg" capture segment files.

This tool mirrors the on-disk layout frozen in src/core/format.hpp (version 1.0):

    FileHeader (512 B) | FrameRecord | FrameRecord | ...

where each FrameRecord is

    FrameHeader (96 B) | payload (payload_size B) | zero padding

Every record starts at a multiple of record_align, exactly as written by src/core/recorder.cpp: the
first record at align_up(512, record_align), each next one align_up(96 + payload_size, record_align)
bytes later. record_align == 1 packs records back to back right after the file header. All integers
are little-endian. Any layout change requires a version bump in format.hpp and here.

Subcommands:
    list          print a per-frame table for one segment
    verify        integrity-check segment(s) plus their idx.jsonl / segments.jsonl side files
    rebuild-index write a separate rebuilt index, preserving matching original SDK metadata
    extract       dump a single frame's raw payload

Exit codes: 0 clean, 1 integrity errors (or frame not found), 2 usage errors.
Python 3.8+, standard library only. Doubles as the seed for offline unpacking scripts.
"""

import argparse
import json
import os
import struct
import sys

# ----------------------------------------------------------------------------------------------------
# CRC-32C (Castagnoli): polynomial 0x1EDC6F41, reflected, init/xorout 0xFFFFFFFF.
# zlib.crc32 is CRC-32/ISO-HDLC and would NOT match; a pure-python table implementation is used instead.
# ----------------------------------------------------------------------------------------------------


# Legacy command helpers re-export the same format primitives used by previews.
if __package__:
    from .jai_raw.layout import *  # noqa: F401,F403
    from .jai_raw.pfnc import nominal_payload_bytes
else:
    from jai_raw.layout import *  # noqa: F401,F403
    from jai_raw.pfnc import nominal_payload_bytes


def open_segment(path):
    """Opens a segment for reading; returns (file, size). Exits with code 2 on usage errors."""
    if not os.path.isfile(path):
        print("error: no such file: %s" % path, file=sys.stderr)
        sys.exit(2)
    f = open(path, "rb")
    size = os.fstat(f.fileno()).st_size
    return f, size


def header_or_fallback(f, path, size, strict):
    """Parses the file header; on damage either exits (strict) or falls back to defaults with a warning."""
    fh, problems = read_file_header(f, path, size)
    if not problems:
        return fh
    for p in problems:
        print("warning: %s: %s" % (path, p), file=sys.stderr)
    if strict or fh is None:
        print("error: %s: unusable file header" % path, file=sys.stderr)
        sys.exit(1)
    align = fh.align if fh.align >= 1 else 4096
    print("warning: %s: continuing with record_align=%d" % (path, align), file=sys.stderr)
    return fh._replace(align=align)


# ----------------------------------------------------------------------------------------------------
# list
# ----------------------------------------------------------------------------------------------------


def cmd_list(args):
    f, size = open_segment(args.segment)
    with f:
        fh = header_or_fallback(f, args.segment, size, strict=False)
        print("# %s: segment %d, camera_id=%s serial=%s, record_align=%d, segment_flags=0x%X"
              % (args.segment, fh.seg_index, cstr(fh.cam_id), cstr(fh.cam_serial), fh.align, fh.flags))
        print("%8s %12s %20s %10s %11s %10s %10s %12s"
              % ("seq", "bid", "dts", "pf", "WxH", "psz", "flags", "off"))
        shown = 0
        for ev in iter_records(f, size, align_up(FILE_HEADER_SIZE, fh.align), fh.align):
            if ev[0] == "frame":
                fr = ev[1]
                print("%8d %12d %20d 0x%08X %5dx%-5d %10d %10s %12d"
                      % (fr.seq, fr.bid, fr.dts, fr.pf, fr.w, fr.h, fr.psz, flags_str(fr.fl), fr.off))
                shown += 1
                if args.limit and shown >= args.limit:
                    break
            elif ev[0] == "corrupt":
                print("# corrupt bytes at offset %d (%s), resynced at %d" % (ev[1], ev[3], ev[2]),
                      file=sys.stderr)
            else:
                print("# truncated tail at offset %d: %s" % (ev[1], ev[2]), file=sys.stderr)
    return 0


# ----------------------------------------------------------------------------------------------------
# verify
# ----------------------------------------------------------------------------------------------------


class Reporter:
    def __init__(self):
        self.errors = 0
        self.warnings = 0

    def error(self, msg):
        self.errors += 1
        print("ERROR %s" % msg)

    def warn(self, msg):
        self.warnings += 1
        print("WARN  %s" % msg)

    def info(self, msg):
        print("INFO  %s" % msg)

    def ok(self, msg):
        print("OK    %s" % msg)


def payload_crc_of(f, off, psz):
    crc = 0
    pos = off + FRAME_HEADER_SIZE
    end = pos + psz
    while pos < end:
        f.seek(pos)
        buf = f.read(min(4 << 20, end - pos))
        if not buf:
            break
        crc = crc32c(buf, crc)
        pos += len(buf)
    return crc


def load_index(path, rep):
    """Parses an idx.jsonl. A truncated final line is discarded (crash artifact); bad middle lines are
    integrity errors. Returns the list of entries (dicts)."""
    with open(path, "rb") as f:
        raw = f.read()
    entries = []
    lines = raw.split(b"\n")
    trailing = lines.pop() if lines else b""  # bytes after the final newline; empty on a clean file
    for i, line in enumerate(lines):
        if not line:
            rep.error("%s: empty line %d in index" % (path, i + 1))
            continue
        try:
            entries.append(json.loads(line.decode("utf-8")))
        except (ValueError, UnicodeDecodeError):
            rep.error("%s: unparseable index line %d" % (path, i + 1))
    if trailing:
        try:
            entries.append(json.loads(trailing.decode("utf-8")))
            rep.info("%s: final index line lacks a newline (accepted)" % path)
        except (ValueError, UnicodeDecodeError):
            rep.info("%s: discarded truncated final index line (crash artifact)" % path)
    return entries


def load_segments_jsonl(path, rep):
    """Returns {seg_name: entry}. A truncated final line is discarded with a note."""
    summaries = {}
    if not os.path.isfile(path):
        return summaries
    with open(path, "rb") as f:
        raw = f.read()
    lines = raw.split(b"\n")
    trailing = lines.pop() if lines else b""
    for i, line in enumerate(lines):
        if not line:
            continue
        try:
            entry = json.loads(line.decode("utf-8"))
            summaries[entry.get("seg", "")] = entry
        except (ValueError, UnicodeDecodeError):
            rep.error("%s: unparseable line %d" % (path, i + 1))
    if trailing:
        try:
            entry = json.loads(trailing.decode("utf-8"))
            summaries[entry.get("seg", "")] = entry
        except (ValueError, UnicodeDecodeError):
            rep.info("%s: discarded truncated final line (crash artifact)" % path)
    return summaries


def expected_payload_size(fr):
    """Use the same packing registry as native decoding (including legacy GVSP)."""
    return nominal_payload_bytes(fr.pf, fr.w, fr.h)


def verify_segment(path, rep, index_path=None):
    """Verifies one segment plus its idx.jsonl. Returns (frames, file_size, truncated) for group checks."""
    f, size = open_segment(path)
    frames = []
    truncated = False
    with f:
        fh, problems = read_file_header(f, path, size)
        for p in problems:
            rep.error("%s: %s" % (path, p))
        if fh is None:
            return frames, size, truncated
        align = fh.align if fh.align >= 1 else 4096
        check_payload_crc = bool(fh.flags & SEG_FLAG_PAYLOAD_CRC)
        if fh.flags & SEG_FLAG_CHUNK_DATA:
            rep.warn("%s: segment_flags has CHUNK_DATA set, which is reserved in v1" % path)

        gaps = []
        dts_backwards = 0
        pfnc_mismatch = []
        incomplete = 0
        prev = None
        for ev in iter_records(f, size, align_up(FILE_HEADER_SIZE, align), align):
            if ev[0] == "corrupt":
                rep.error("%s: corrupt bytes at offset %d (%s); resynced at offset %d (skipped %d bytes)"
                          % (path, ev[1], ev[3], ev[2], ev[2] - ev[1]))
                continue
            if ev[0] == "trunc":
                truncated = True
                rep.info("%s: truncated tail at offset %d: %s -- crash artifact, run rebuild-index"
                         % (path, ev[1], ev[2]))
                continue
            fr = ev[1]
            frames.append(fr)
            if check_payload_crc and payload_crc_of(f, fr.off, fr.psz) != fr.pcrc:
                rep.error("%s: payload CRC-32C mismatch for frame seq=%d at offset %d"
                          % (path, fr.seq, fr.off))
            if fr.fl & FRAME_FLAG_INCOMPLETE:
                incomplete += 1
            elif fr.psz != expected_payload_size(fr):
                pfnc_mismatch.append(fr)
            if prev is not None:
                if fr.bid > prev.bid + 1:
                    gaps.append((prev.bid, fr.bid))
                if fr.dts <= prev.dts:
                    dts_backwards += 1
            prev = fr

        if gaps:
            first = ", ".join("%d->%d" % g for g in gaps[:3])
            rep.warn("%s: %d block_id gap(s) (network loss): %s%s"
                     % (path, len(gaps), first, ", ..." if len(gaps) > 3 else ""))
        if dts_backwards:
            rep.warn("%s: device_ts_ns not strictly increasing for %d frame(s)" % (path, dts_backwards))
        if pfnc_mismatch:
            fr = pfnc_mismatch[0]
            rep.warn("%s: %d frame(s) where payload_size != PFNC expectation (e.g. seq=%d: %d vs %d); "
                     "GVSP legacy packed formats legitimately differ"
                     % (path, len(pfnc_mismatch), fr.seq, fr.psz, expected_payload_size(fr)))
        if incomplete:
            rep.info("%s: %d frame(s) flagged INCOMPLETE (recorded with missing packets)" % (path, incomplete))

        # Cross-check against the index: it must be an exact per-frame prefix of the data.
        idx_path = index_path or (path[:-4] + ".idx.jsonl" if path.endswith(".raw") else path + ".idx.jsonl")
        if os.path.isfile(idx_path):
            entries = load_index(idx_path, rep)
            if len(entries) > len(frames):
                rep.error("%s: index has %d entries but segment has only %d complete frame(s); "
                          "index must be a subset of the data" % (idx_path, len(entries), len(frames)))
            unknown_layout = 0
            for i, (e, fr) in enumerate(zip(entries, frames)):
                if not isinstance(e, dict):
                    rep.error("%s: entry %d is not a JSON object" % (idx_path, i))
                    continue
                diffs = []
                for key, actual in (("off", fr.off), ("psz", fr.psz), ("bid", fr.bid),
                                    ("dts", fr.dts), ("fl", fr.fl), ("seq", fr.seq),
                                    ("hrt", fr.hrt), ("hmn", fr.hmn), ("pf", fr.pf),
                                    ("w", fr.w), ("h", fr.h), ("ox", fr.ox), ("oy", fr.oy)):
                    if key in ("ox", "oy") and key not in e:
                        continue # legacy indexes omitted ROI offsets; raw headers retain them
                    if type(e.get(key)) is not int or e[key] != actual:
                        diffs.append("%s: index=%s data=%s" % (key, e.get(key), actual))
                if diffs:
                    rep.error("%s: entry %d disagrees with data (%s)" % (idx_path, i, "; ".join(diffs)))
                if any(e.get(key) is None for key in
                       ("payload_type", "padding_x", "padding_y", "chunk_count", "operation_result")):
                    unknown_layout += 1
            if unknown_layout:
                rep.warn("%s: %d frame(s) lack SDK-only layout metadata; v1 raw headers cannot recover it"
                         % (idx_path, unknown_layout))
            if len(entries) < len(frames):
                rep.info("%s: %d data frame(s) beyond the index tail (recoverable via rebuild-index)"
                         % (path, len(frames) - len(entries)))
        else:
            rep.info("%s: no index file (%s)" % (path, idx_path))

        rep.ok("%s: %d frame(s), %d bytes, segment_flags=0x%X" % (path, len(frames), size, fh.flags))
    return frames, size, truncated


def check_against_summary(path, entry, frames, size, truncated, rep):
    name = os.path.basename(path)
    if entry is None:
        rep.info("%s: no segments.jsonl entry (expected for a segment open at crash/kill time)" % name)
        return
    if not entry.get("closed_clean", False):
        rep.info("%s: segments.jsonl says closed_clean=false; skipping totals comparison" % name)
        return
    if truncated:
        rep.error("%s: segments.jsonl says closed_clean=true but the segment has a truncated tail" % name)
    diffs = []
    checks = [("frames", len(frames)), ("bytes", size)]
    if frames:
        checks += [("seq_first", frames[0].seq), ("seq_last", frames[-1].seq),
                   ("bid_first", frames[0].bid), ("bid_last", frames[-1].bid),
                   ("dts_first", frames[0].dts), ("dts_last", frames[-1].dts)]
    for key, actual in checks:
        if key in entry and entry[key] != actual:
            diffs.append("%s: summary=%s actual=%s" % (key, entry[key], actual))
    if diffs:
        rep.error("%s: segments.jsonl disagrees with data (%s)" % (name, "; ".join(diffs)))
    else:
        rep.ok("%s: segments.jsonl totals match" % name)



def cmd_verify(args):
    if args.index and (not os.path.isfile(args.path) or not os.path.isfile(args.index)):
        print("error: --index requires one existing segment and one existing index file", file=sys.stderr)
        return 2
    groups = collect_targets(args.path)
    if groups is None:
        print("error: no such file or directory: %s" % args.path, file=sys.stderr)
        return 2
    if not groups:
        print("error: no seg_*.raw files found under %s" % args.path, file=sys.stderr)
        return 2
    rep = Reporter()
    total_frames = 0
    total_segments = 0
    for d in sorted(groups):
        summaries = load_segments_jsonl(os.path.join(d, "segments.jsonl"), rep)
        for seg in sorted(groups[d]):
            frames, size, truncated = verify_segment(seg, rep, args.index)
            check_against_summary(seg, summaries.get(os.path.basename(seg)), frames, size, truncated, rep)
            total_frames += len(frames)
            total_segments += 1
    print("verify: %d segment(s), %d frame(s), %d error(s), %d warning(s)"
          % (total_segments, total_frames, rep.errors, rep.warnings))
    return 1 if rep.errors else 0


# ----------------------------------------------------------------------------------------------------
# rebuild-index
# ----------------------------------------------------------------------------------------------------


def recovered_index_entry(fr, original=None):
    """Header fields are authoritative; SDK-only fields require a matching original row."""
    header = {key: getattr(fr, key) for key in
              ("seq", "bid", "dts", "hrt", "hmn", "off", "psz", "pf", "w", "h", "fl", "ox", "oy")}
    # Older indexes lack ox/oy; all other identity fields must match before any
    # supplementary metadata can be associated with the recovered payload.
    matched = isinstance(original, dict) and all(
        (key in ("ox", "oy") and key not in original) or
        (type(original.get(key)) is int and original[key] == value)
        for key, value in header.items())
    row = dict(original) if matched else {}
    row.update(header)
    for key in ("payload_type", "padding_x", "padding_y", "chunk_count", "operation_result"):
        row.setdefault(key, None)  # unknown is never represented as a valid zero
    return row, matched


def format_index_line(fr, original=None):
    row, _ = recovered_index_entry(fr, original)
    return (json.dumps(row, separators=(",", ":")) + "\n").encode("utf-8")


def recovery_source_rows(segment):
    """Keep the original index read-only; ambiguous offsets cannot supply metadata."""
    path = os.path.splitext(segment)[0] + ".idx.jsonl"
    if not os.path.isfile(path):
        return {}
    rows = {}
    with open(path, "rb") as source:
        for line_number, line in enumerate(source, 1):
            try:
                row = json.loads(line)
                if not isinstance(row, dict) or type(row.get("off")) is not int:
                    raise ValueError("index row has no integer offset")
            except (ValueError, UnicodeDecodeError) as exc:
                print("warning: %s: skipping invalid index line %d (%s)"
                      % (path, line_number, exc), file=sys.stderr)
                continue
            off = row["off"]
            if off in rows:
                rows[off] = None  # duplicate identity: do not guess which row is authoritative
                print("warning: %s: duplicate offset %d; SDK metadata left unknown"
                      % (path, off), file=sys.stderr)
            else:
                rows[off] = row
    return rows


def _rebuild_one(segment, output):
    """Never replaces a file. output=None writes to stdout. Returns the line count."""
    if output and os.path.lexists(output):
        raise FileExistsError("refusing to overwrite existing output: " + output)
    originals = recovery_source_rows(segment)
    f, size = open_segment(segment)
    with f:
        fh = header_or_fallback(f, segment, size, strict=False)
        part = output + ".part" if output else None
        out = open(part, "xb") if part else sys.stdout.buffer
        try:
            written = 0
            unassociated = 0
            for ev in iter_records(f, size, align_up(FILE_HEADER_SIZE, fh.align), fh.align):
                if ev[0] == "frame":
                    fr = ev[1]
                    original = originals.get(fr.off)
                    _, matched = recovered_index_entry(fr, original)
                    if not matched:
                        unassociated += 1
                    out.write(format_index_line(fr, original))
                    written += 1
                elif ev[0] == "corrupt":
                    print("warning: %s: corrupt bytes at offset %d (%s), resynced at %d"
                          % (segment, ev[1], ev[3], ev[2]), file=sys.stderr)
                else:
                    print("note: %s: truncated tail at offset %d: %s" % (segment, ev[1], ev[2]),
                          file=sys.stderr)
            if output:
                out.flush()
                os.fsync(out.fileno())
                out.close()
                os.link(part, output)  # atomic publication, fails if output now exists
                os.unlink(part)
                dir_fd = os.open(os.path.dirname(os.path.abspath(output)), os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(dir_fd)
                finally:
                    os.close(dir_fd)
        except BaseException:
            if output:
                try:
                    out.close()
                finally:
                    if os.path.exists(part):
                        os.unlink(part)
            raise
        print("rebuild-index: %s: %d index line(s)%s"
              % (segment, written, " -> " + output if output else ""), file=sys.stderr)
        if unassociated:
            print("warning: %d frame(s) have no matching original index row; SDK-only metadata "
                  "is unknown and cannot be reconstructed from v1 raw headers" % unassociated,
                  file=sys.stderr)
    return written


def cmd_rebuild_index(args):
    if os.path.isdir(args.path):
        # Keep original indexes intact: they contain fields absent from v1 raw
        # headers. Each result is separate and must not overwrite an earlier recovery.
        if args.output:
            print("error: -o/--output only applies when a single seg_NNNNN.raw is given",
                  file=sys.stderr)
            return 2
        groups = collect_targets(args.path)
        if not groups:
            print("error: no seg_*.raw files found under %s" % args.path, file=sys.stderr)
            return 2
        total = 0
        for d in sorted(groups):
            for seg in sorted(groups[d]):
                idx_path = seg[:-len(".raw")] + ".rebuilt.idx.jsonl"
                total += _rebuild_one(seg, idx_path)
        print("rebuild-index: done (%d frame(s) indexed); original indexes unchanged; "
              "review the separate .rebuilt.idx.jsonl files" % total,
              file=sys.stderr)
        return 0
    if not os.path.isfile(args.path):
        print("error: no such file or directory: %s" % args.path, file=sys.stderr)
        return 2
    _rebuild_one(args.path, args.output)
    return 0


# ----------------------------------------------------------------------------------------------------
# extract
# ----------------------------------------------------------------------------------------------------


def cmd_extract(args):
    f, size = open_segment(args.segment)
    with f:
        fh = header_or_fallback(f, args.segment, size, strict=False)
        for ev in iter_records(f, size, align_up(FILE_HEADER_SIZE, fh.align), fh.align):
            if ev[0] != "frame" or ev[1].seq != args.seq:
                continue
            fr = ev[1]
            with open(args.output, "wb") as out:
                pos = fr.off + FRAME_HEADER_SIZE
                end = pos + fr.psz
                while pos < end:
                    f.seek(pos)
                    buf = f.read(min(4 << 20, end - pos))
                    if not buf:
                        print("error: short read inside payload", file=sys.stderr)
                        return 1
                    out.write(buf)
                    pos += len(buf)
            print("extract: frame seq=%d bid=%d pf=0x%08X %dx%d flags=%s -> %s (%d bytes)"
                  % (fr.seq, fr.bid, fr.pf, fr.w, fr.h, flags_str(fr.fl), args.output, fr.psz),
                  file=sys.stderr)
            return 0
    print("error: frame seq=%d not found in %s" % (args.seq, args.segment), file=sys.stderr)
    return 1


# ----------------------------------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="inspect_raw.py",
        description="Inspect/verify/repair jai-raw-seg capture segments (format v1, src/core/format.hpp).")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="print a per-frame table for one segment")
    p.add_argument("segment", help="path to a seg_NNNNN.raw file")
    p.add_argument("--limit", type=int, default=0, metavar="N", help="stop after N frames (default: all)")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("verify", help="integrity-check segment(s) and their index/summary side files")
    p.add_argument("path", help="a seg_NNNNN.raw file, a camera directory, or a session directory")
    p.add_argument("--index", metavar="INDEX.jsonl",
                   help="verify a separate rebuilt index against one segment, without replacing the original")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("rebuild-index",
                       help="recover a separate index, retaining matching original SDK metadata")
    p.add_argument("path", help="a seg_NNNNN.raw file (prints to stdout unless -o), or a camera/"
                                "session directory (creates separate .rebuilt.idx.jsonl files)")
    p.add_argument("-o", "--output", metavar="OUT.jsonl",
                   help="new output file, never overwritten (single-file mode; default: stdout)")
    p.set_defaults(func=cmd_rebuild_index)

    p = sub.add_parser("extract", help="dump one frame's raw payload bytes")
    p.add_argument("segment", help="path to a seg_NNNNN.raw file")
    p.add_argument("--seq", type=int, required=True, help="frame_seq of the frame to extract")
    p.add_argument("-o", "--output", required=True, metavar="OUT.bin", help="output file")
    p.set_defaults(func=cmd_extract)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (OSError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
