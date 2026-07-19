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
    rebuild-index regenerate seg_NNNNN.idx.jsonl from the data (crash recovery; resync scan)
    extract       dump a single frame's raw payload

Exit codes: 0 clean, 1 integrity errors (or frame not found), 2 usage errors.
Python 3.8+, standard library only. Doubles as the seed for offline unpacking scripts.
"""

import argparse
import json
import os
import struct
import sys
from collections import namedtuple

# ----------------------------------------------------------------------------------------------------
# CRC-32C (Castagnoli): polynomial 0x1EDC6F41, reflected, init/xorout 0xFFFFFFFF.
# zlib.crc32 is CRC-32/ISO-HDLC and would NOT match; a pure-python table implementation is used instead.
# ----------------------------------------------------------------------------------------------------


def _make_crc32c_table():
    poly = 0x82F63B78  # 0x1EDC6F41 bit-reflected
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
        table.append(crc)
    return table


_CRC32C_TABLE = _make_crc32c_table()


def crc32c(data, seed=0):
    """CRC-32C of `data`; pass the previous return value as `seed` to checksum incrementally."""
    crc = ~seed & 0xFFFFFFFF
    table = _CRC32C_TABLE
    for b in data:
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return ~crc & 0xFFFFFFFF


# Unit self-check with the standard test vector (explicit raise: survives `python -O`).
if crc32c(b"123456789") != 0xE3069283:
    raise AssertionError("CRC-32C self-check failed: b'123456789' must hash to 0xE3069283")


# ----------------------------------------------------------------------------------------------------
# Layout constants (must match src/core/format.hpp exactly)
# ----------------------------------------------------------------------------------------------------

FILE_MAGIC = b"JAIRAWSG"
FRAME_MAGIC = 0x4D415246  # little-endian bytes "FRAM"
FRAME_MAGIC_BYTES = b"FRAM"
BYTE_ORDER_MARK = 0x0A0B0C0D
VERSION_MAJOR = 1

FILE_HEADER_SIZE = 512
FRAME_HEADER_SIZE = 96
FRAME_HEADER_CRC_OFF = 92  # header_crc32c covers bytes [0, 92)
FILE_HEADER_CRC_OFF = 508  # header_crc32c covers bytes [0, 508)

# FileHeader: magic[8] u32 u16 u16 u32 u32 u64 uuid[16] cam_id[64] serial[64] u32 u32 u32 res[320] u32
FILE_HEADER_FMT = "<8sIHHIIQ16s64s64sIII320sI"
# FrameHeader: magic hsize | bid dts hrt hmn | pf w h ox oy fl | psz seq | pcrc r0 r1 hcrc
FRAME_HEADER_FMT = "<2I4Q6I2Q4I"

if struct.calcsize(FILE_HEADER_FMT) != FILE_HEADER_SIZE:
    raise AssertionError("FILE_HEADER_FMT does not pack to 512 bytes")
if struct.calcsize(FRAME_HEADER_FMT) != FRAME_HEADER_SIZE:
    raise AssertionError("FRAME_HEADER_FMT does not pack to 96 bytes")

SEG_FLAG_CHUNK_DATA = 1 << 0
SEG_FLAG_PAYLOAD_CRC = 1 << 1

FRAME_FLAG_INCOMPLETE = 1 << 0
FRAME_FLAG_RESULT_NOT_OK = 1 << 1
FRAME_FLAG_BLOCKID_GAP = 1 << 2
FRAME_FLAG_TS_SUSPECT = 1 << 3
FRAME_FLAG_CHUNK_DATA = 1 << 4

_FLAG_LETTERS = (
    (FRAME_FLAG_INCOMPLETE, "I"),
    (FRAME_FLAG_RESULT_NOT_OK, "R"),
    (FRAME_FLAG_BLOCKID_GAP, "G"),
    (FRAME_FLAG_TS_SUSPECT, "T"),
    (FRAME_FLAG_CHUNK_DATA, "C"),
)

FileHeader = namedtuple(
    "FileHeader",
    "magic hsize vmaj vmin bom seg_index created_ns uuid cam_id cam_serial fhsize align flags reserved crc",
)

# `off` is the absolute file offset of the record (prepended to the packed header fields).
Frame = namedtuple("Frame", "off magic hsize bid dts hrt hmn pf w h ox oy fl psz seq pcrc r0 r1 hcrc")


def align_up(value, align):
    return value if align <= 1 else (value + align - 1) // align * align


def flags_str(fl):
    letters = "".join(ch for bit, ch in _FLAG_LETTERS if fl & bit)
    return "0x%02X%s" % (fl, "(" + letters + ")" if letters else "")


def cstr(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


# ----------------------------------------------------------------------------------------------------
# Header parsing
# ----------------------------------------------------------------------------------------------------


def parse_file_header(buf):
    """Returns (FileHeader, [problem strings]). An empty problem list means the header is trustworthy."""
    fh = FileHeader(*struct.unpack(FILE_HEADER_FMT, buf))
    problems = []
    if fh.magic != FILE_MAGIC:
        problems.append("bad file magic %r (expected %r)" % (fh.magic, FILE_MAGIC))
    if fh.hsize != FILE_HEADER_SIZE:
        problems.append("file_header_size=%d (expected %d)" % (fh.hsize, FILE_HEADER_SIZE))
    if fh.bom != BYTE_ORDER_MARK:
        problems.append("byte_order_mark=0x%08X (expected 0x%08X)" % (fh.bom, BYTE_ORDER_MARK))
    if fh.vmaj != VERSION_MAJOR:
        problems.append("unsupported format version %d.%d" % (fh.vmaj, fh.vmin))
    if crc32c(buf[:FILE_HEADER_CRC_OFF]) != fh.crc:
        problems.append("file header CRC-32C mismatch")
    if fh.fhsize != FRAME_HEADER_SIZE:
        problems.append("frame_header_size=%d (expected %d)" % (fh.fhsize, FRAME_HEADER_SIZE))
    if fh.align < 1:
        problems.append("record_align=%d is invalid" % fh.align)
    return fh, problems


def frame_header_ok(buf):
    """Validates one candidate 96-byte frame header. Returns None if valid, else a reason string."""
    magic, hsize = struct.unpack_from("<2I", buf)
    if magic != FRAME_MAGIC:
        return "bad frame magic 0x%08X" % magic
    if hsize != FRAME_HEADER_SIZE:
        return "unsupported frame header_size %d" % hsize
    stored = struct.unpack_from("<I", buf, FRAME_HEADER_CRC_OFF)[0]
    if crc32c(buf[:FRAME_HEADER_CRC_OFF]) != stored:
        return "frame header CRC-32C mismatch"
    return None


def read_file_header(f, path, size):
    """Reads + parses the file header; returns (FileHeader or None, problems). Usage-level failures raise."""
    if size < FILE_HEADER_SIZE:
        return None, ["file is only %d byte(s), shorter than the 512-byte file header" % size]
    f.seek(0)
    return parse_file_header(f.read(FILE_HEADER_SIZE))


# ----------------------------------------------------------------------------------------------------
# Record walking with crash-tolerant resync
# ----------------------------------------------------------------------------------------------------


def _scan_magic(f, start, size):
    """Yields every absolute offset >= start where the 4-byte frame magic appears (chunked scan)."""
    chunk_size = 1 << 20
    pos = start
    carry = b""
    while pos < size:
        f.seek(pos)
        buf = f.read(min(chunk_size, size - pos))
        if not buf:
            return
        data = carry + buf
        base = pos - len(carry)
        i = data.find(FRAME_MAGIC_BYTES)
        while i != -1:
            yield base + i
            i = data.find(FRAME_MAGIC_BYTES, i + 1)
        carry = data[-(len(FRAME_MAGIC_BYTES) - 1):]
        pos += len(buf)


def _find_resync(f, start, size):
    """First offset >= start holding a fully valid frame header, or None."""
    for cand in _scan_magic(f, start, size):
        if cand + FRAME_HEADER_SIZE > size:
            return None
        f.seek(cand)
        if frame_header_ok(f.read(FRAME_HEADER_SIZE)) is None:
            return cand
    return None


def _all_zero(f, start, size):
    pos = start
    while pos < size:
        f.seek(pos)
        buf = f.read(min(1 << 20, size - pos))
        if not buf:
            break
        if buf.count(0) != len(buf):
            return False
        pos += len(buf)
    return True


def iter_records(f, size, start, record_align):
    """Walks frame records from `start`, resyncing on damage. Yields event tuples:

    ("frame",   Frame)                          a record with a valid header and complete payload
    ("corrupt", off, resumed_at, reason)        damaged bytes mid-file; walking resumed at resumed_at
    ("trunc",   off, reason)                    incomplete tail (always the last event when emitted)
    """
    pos = start
    while pos < size:
        avail = size - pos
        if avail < FRAME_HEADER_SIZE:
            yield ("trunc", pos, "partial frame header at tail (%d byte(s))" % avail)
            return
        f.seek(pos)
        buf = f.read(FRAME_HEADER_SIZE)
        reason = frame_header_ok(buf)
        if reason is None:
            fr = Frame(pos, *struct.unpack(FRAME_HEADER_FMT, buf))
            record_bytes = align_up(FRAME_HEADER_SIZE + fr.psz, record_align)
            if pos + FRAME_HEADER_SIZE + fr.psz > size:
                missing = pos + FRAME_HEADER_SIZE + fr.psz - size
                yield ("trunc", pos, "frame seq=%d payload truncated (%d byte(s) missing)" % (fr.seq, missing))
                return
            yield ("frame", fr)
            if pos + record_bytes > size:
                yield ("trunc", pos, "padding of final record truncated (payload complete)")
                return
            pos += record_bytes
            continue
        resumed = _find_resync(f, pos + 1, size)
        if resumed is None:
            if _all_zero(f, pos, size):
                reason += "; zero-filled tail (torn final write)"
            else:
                reason += "; no further valid frame header until EOF"
            yield ("trunc", pos, reason)
            return
        yield ("corrupt", pos, resumed, reason)
        pos = resumed


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
    """PFNC-derived expected payload size: bpp lives in bits [16,24) of the pixel format code."""
    bpp = (fr.pf >> 16) & 0xFF
    return (fr.w * fr.h * bpp + 7) // 8


def verify_segment(path, rep):
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
        idx_path = path[:-4] + ".idx.jsonl" if path.endswith(".raw") else path + ".idx.jsonl"
        if os.path.isfile(idx_path):
            entries = load_index(idx_path, rep)
            if len(entries) > len(frames):
                rep.error("%s: index has %d entries but segment has only %d complete frame(s); "
                          "index must be a subset of the data" % (idx_path, len(entries), len(frames)))
            for i, (e, fr) in enumerate(zip(entries, frames)):
                diffs = []
                for key, actual in (("off", fr.off), ("psz", fr.psz), ("bid", fr.bid),
                                    ("dts", fr.dts), ("fl", fr.fl), ("seq", fr.seq)):
                    if key in e and e[key] != actual:
                        diffs.append("%s: index=%s data=%s" % (key, e[key], actual))
                if diffs:
                    rep.error("%s: entry %d disagrees with data (%s)" % (idx_path, i, "; ".join(diffs)))
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


def collect_targets(path):
    """Maps a file or session/camera directory to {directory: [segment paths]}."""
    if os.path.isfile(path):
        return {os.path.dirname(path) or ".": [path]}
    if os.path.isdir(path):
        groups = {}
        candidates = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            if name.startswith("seg_") and name.endswith(".raw") and os.path.isfile(full):
                candidates.append(full)
            elif os.path.isdir(full):
                for sub in sorted(os.listdir(full)):
                    if sub.startswith("seg_") and sub.endswith(".raw"):
                        candidates.append(os.path.join(full, sub))
        for seg in candidates:
            groups.setdefault(os.path.dirname(seg), []).append(seg)
        return groups
    return None


def cmd_verify(args):
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
            frames, size, truncated = verify_segment(seg, rep)
            check_against_summary(seg, summaries.get(os.path.basename(seg)), frames, size, truncated, rep)
            total_frames += len(frames)
            total_segments += 1
    print("verify: %d segment(s), %d frame(s), %d error(s), %d warning(s)"
          % (total_segments, total_frames, rep.errors, rep.warnings))
    return 1 if rep.errors else 0


# ----------------------------------------------------------------------------------------------------
# rebuild-index
# ----------------------------------------------------------------------------------------------------


def format_index_line(fr):
    """Byte-identical to the snprintf in Recorder::write_frame (src/core/recorder.cpp)."""
    return ('{"seq":%d,"bid":%d,"dts":%d,"hrt":%d,"hmn":%d,"off":%d,"psz":%d,"pf":%d,"w":%d,"h":%d,'
            '"fl":%d}\n' % (fr.seq, fr.bid, fr.dts, fr.hrt, fr.hmn, fr.off, fr.psz, fr.pf, fr.w, fr.h,
                            fr.fl)).encode("ascii")


def _rebuild_one(segment, output):
    """Rebuilds one segment's index. output=None writes to stdout. Returns the line count."""
    f, size = open_segment(segment)
    with f:
        fh = header_or_fallback(f, segment, size, strict=False)
        out = open(output, "wb") if output else sys.stdout.buffer
        try:
            written = 0
            for ev in iter_records(f, size, align_up(FILE_HEADER_SIZE, fh.align), fh.align):
                if ev[0] == "frame":
                    out.write(format_index_line(ev[1]))
                    written += 1
                elif ev[0] == "corrupt":
                    print("warning: %s: corrupt bytes at offset %d (%s), resynced at %d"
                          % (segment, ev[1], ev[3], ev[2]), file=sys.stderr)
                else:
                    print("note: %s: truncated tail at offset %d: %s" % (segment, ev[1], ev[2]),
                          file=sys.stderr)
        finally:
            if output:
                out.close()
        print("rebuild-index: %s: %d index line(s)%s"
              % (segment, written, " -> " + output if output else ""), file=sys.stderr)
    return written


def cmd_rebuild_index(args):
    if os.path.isdir(args.path):
        # Directory form (crash recovery): rebuild EVERY segment's index
        # in place, next to its .raw file. Accepts a camera directory or a
        # whole session directory.
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
                idx_path = seg[:-len(".raw")] + ".idx.jsonl"
                total += _rebuild_one(seg, idx_path)
        print("rebuild-index: done (%d frame(s) indexed); re-run verify to confirm" % total,
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
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("rebuild-index",
                       help="regenerate idx.jsonl from segment data (crash recovery; resync scan)")
    p.add_argument("path", help="a seg_NNNNN.raw file (prints to stdout unless -o), or a camera/"
                                "session directory (rewrites every idx.jsonl in place)")
    p.add_argument("-o", "--output", metavar="OUT.jsonl",
                   help="output file (single-file mode only; default: stdout)")
    p.set_defaults(func=cmd_rebuild_index)

    p = sub.add_parser("extract", help="dump one frame's raw payload bytes")
    p.add_argument("segment", help="path to a seg_NNNNN.raw file")
    p.add_argument("--seq", type=int, required=True, help="frame_seq of the frame to extract")
    p.add_argument("-o", "--output", required=True, metavar="OUT.bin", help="output file")
    p.set_defaults(func=cmd_extract)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
