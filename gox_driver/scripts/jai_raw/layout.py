"""Pure jai-raw-seg v1 layout, CRC and bounded record walking. No CLI or SDK imports."""

import os
import struct
from collections import namedtuple


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
    if len(buf) != FILE_HEADER_SIZE:
        return None, ["incomplete file header"]
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
    if len(buf) != FRAME_HEADER_SIZE:
        return "incomplete frame header"
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
