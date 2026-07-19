#!/usr/bin/env python3
"""Dev helper -- NOT part of the driver runtime.

Synthesizes a fake capture session directory that is byte-compatible with what src/core/recorder.cpp
writes (layout frozen in src/core/format.hpp, version 1.0), for exercising scripts/inspect_raw.py
without a camera or a C++ build:

    <out>/cam0/seg_00001.raw        4 Mono8 frames (one INCOMPLETE, one after a block_id gap),
                                    payload CRCs enabled, cleanly closed
    <out>/cam0/seg_00001.idx.jsonl  matching index (recorder line format)
    <out>/cam0/seg_00002.raw        2 complete frames + a record whose payload is cut short,
                                    simulating a crash (kill -9) mid-write
    <out>/cam0/seg_00002.idx.jsonl  index for the first frame only + a truncated final line
    <out>/cam0/segments.jsonl       summary line for seg_00001 only (seg_00002 never rotated)

The CRC-32C and struct packing here are implemented independently of inspect_raw.py so the two tools
validate each other. Python 3.8+, standard library only.
"""

import argparse
import os
import struct
import sys

# ----------------------------------------------------------------------------------------------------
# CRC-32C (Castagnoli, reflected, poly 0x1EDC6F41, init/xorout 0xFFFFFFFF) -- independent copy.
# ----------------------------------------------------------------------------------------------------

_TABLE = []
for _i in range(256):
    _crc = _i
    for _ in range(8):
        _crc = (_crc >> 1) ^ 0x82F63B78 if _crc & 1 else _crc >> 1
    _TABLE.append(_crc)


def crc32c(data, seed=0):
    crc = ~seed & 0xFFFFFFFF
    for b in data:
        crc = _TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return ~crc & 0xFFFFFFFF


if crc32c(b"123456789") != 0xE3069283:
    raise AssertionError("CRC-32C self-check failed")

# ----------------------------------------------------------------------------------------------------
# format.hpp layout
# ----------------------------------------------------------------------------------------------------

FILE_HEADER_FMT = "<8sIHHIIQ16s64s64sIII320sI"
FRAME_HEADER_FMT = "<2I4Q6I2Q4I"
assert struct.calcsize(FILE_HEADER_FMT) == 512
assert struct.calcsize(FRAME_HEADER_FMT) == 96

FRAME_MAGIC = 0x4D415246
SEG_FLAG_PAYLOAD_CRC = 1 << 1
FL_INCOMPLETE = 1 << 0
FL_BLOCKID_GAP = 1 << 2

PF_MONO8 = 0x01080001  # PFNC Mono8: bpp field ((pf >> 16) & 0xFF) == 8
WIDTH, HEIGHT = 64, 48
FULL_PSZ = WIDTH * HEIGHT  # 3072 bytes for Mono8
RECORD_ALIGN = 4096
SESSION_UUID = bytes(range(16))
CAMERA_ID = b"cam0"
CAMERA_SERIAL = b"GOX-TEST-0001"
CREATED_NS = 1_752_750_000_000_000_000  # arbitrary plausible CLOCK_REALTIME
DTS0 = 1_752_750_000_123_456_789  # PTP/TAI-ish base device timestamp
HRT0 = DTS0 - 37_000_000_000  # host UTC ~= TAI - 37 s
HMN0 = 98_765_432_100
STEP_NS = 50_000_000  # 20 fps


def align_up(v, a):
    return v if a <= 1 else (v + a - 1) // a * a


def payload_for(seq, psz):
    """Deterministic per-frame payload pattern (checked by the extract test)."""
    return bytes((seq * 7 + i) % 256 for i in range(psz))


def make_file_header(seg_index, seg_flags):
    body = struct.pack("<8sIHHIIQ16s64s64sIII320s", b"JAIRAWSG", 512, 1, 0, 0x0A0B0C0D, seg_index,
                       CREATED_NS, SESSION_UUID, CAMERA_ID, CAMERA_SERIAL, 96, RECORD_ALIGN, seg_flags,
                       b"")
    assert len(body) == 508
    return body + struct.pack("<I", crc32c(body))


def make_frame_header(seq, bid, fl, psz, pcrc):
    dts = DTS0 + seq * STEP_NS
    body = struct.pack("<2I4Q6I2Q3I", FRAME_MAGIC, 96, bid, dts, HRT0 + seq * STEP_NS,
                       HMN0 + seq * STEP_NS, PF_MONO8, WIDTH, HEIGHT, 0, 0, fl, psz, seq, pcrc, 0, 0)
    assert len(body) == 92
    return body + struct.pack("<I", crc32c(body))


def index_line(seq, bid, fl, psz, off):
    """Byte-identical to the snprintf in Recorder::write_frame."""
    dts = DTS0 + seq * STEP_NS
    return ('{"seq":%d,"bid":%d,"dts":%d,"hrt":%d,"hmn":%d,"off":%d,"psz":%d,"pf":%d,"w":%d,"h":%d,'
            '"fl":%d}\n' % (seq, bid, dts, HRT0 + seq * STEP_NS, HMN0 + seq * STEP_NS, off, psz,
                            PF_MONO8, WIDTH, HEIGHT, fl)).encode("ascii")


def write_segment(cam_dir, seg_index, frames, truncate_last_payload_to=None):
    """frames: list of (seq, bid, fl, psz). Returns (file size, [index lines], per-frame dts list).

    When truncate_last_payload_to is set, the final frame's record is cut mid-payload after `n` bytes
    (header fully written first, exactly like the recorder's writev ordering on a crash)."""
    seg_path = os.path.join(cam_dir, "seg_%05d.raw" % seg_index)
    idx_lines = []
    with open(seg_path, "wb") as f:
        f.write(make_file_header(seg_index, SEG_FLAG_PAYLOAD_CRC))
        # Records start on RECORD_ALIGN boundaries; pad after the 512-byte
        # file header exactly like Recorder::open_segment.
        off = align_up(512, RECORD_ALIGN)
        f.write(b"\0" * (off - 512))
        for n, (seq, bid, fl, psz) in enumerate(frames):
            payload = payload_for(seq, psz)
            header = make_frame_header(seq, bid, fl, psz, crc32c(payload))
            last = n == len(frames) - 1
            if last and truncate_last_payload_to is not None:
                f.write(header)
                f.write(payload[:truncate_last_payload_to])
                off += 96 + truncate_last_payload_to
                break
            record_bytes = align_up(96 + psz, RECORD_ALIGN)
            f.write(header)
            f.write(payload)
            f.write(b"\0" * (record_bytes - 96 - psz))
            idx_lines.append(index_line(seq, bid, fl, psz, off))
            off += record_bytes
    return off, idx_lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out_dir", nargs="?", default="./test_session",
                        help="session directory to create (default: ./test_session)")
    args = parser.parse_args()

    cam_dir = os.path.join(args.out_dir, "cam0")
    os.makedirs(cam_dir, exist_ok=True)

    # seg_00001: clean. seq 1 is INCOMPLETE (short payload); seq 2 follows a block_id gap (102 missing).
    seg1_frames = [
        (0, 100, 0, FULL_PSZ),
        (1, 101, FL_INCOMPLETE, 2000),
        (2, 103, FL_BLOCKID_GAP, FULL_PSZ),
        (3, 104, 0, FULL_PSZ),
    ]
    seg1_bytes, seg1_idx = write_segment(cam_dir, 1, seg1_frames)
    with open(os.path.join(cam_dir, "seg_00001.idx.jsonl"), "wb") as f:
        f.write(b"".join(seg1_idx))

    # seg_00002: crash simulation. Two complete records, then a record whose payload stops at byte 1000.
    seg2_frames = [
        (4, 105, 0, FULL_PSZ),
        (5, 106, 0, FULL_PSZ),
        (6, 107, 0, FULL_PSZ),
    ]
    write_segment(cam_dir, 2, seg2_frames, truncate_last_payload_to=1000)
    # Its index only got frame seq=4 flushed, plus a partial line cut mid-write.
    with open(os.path.join(cam_dir, "seg_00002.idx.jsonl"), "wb") as f:
        f.write(index_line(4, 105, 0, FULL_PSZ, align_up(512, RECORD_ALIGN)))
        f.write(b'{"seq":5,"bid":106,')

    # segments.jsonl: only seg_00001 rotated cleanly before the "crash". Recorder line format.
    with open(os.path.join(cam_dir, "segments.jsonl"), "wb") as f:
        f.write(('{"seg":"seg_00001.raw","frames":4,"bytes":%d,"seq_first":0,"seq_last":3,'
                 '"bid_first":100,"bid_last":104,"dts_first":%d,"dts_last":%d,"closed_clean":true}\n'
                 % (seg1_bytes, DTS0, DTS0 + 3 * STEP_NS)).encode("ascii"))

    print("generated test session under %s" % os.path.abspath(args.out_dir))
    print("  cam0/seg_00001.raw        clean, 4 frames, %d bytes, payload CRC on" % seg1_bytes)
    print("  cam0/seg_00002.raw        crash-truncated mid-payload of frame seq=6")
    print("  cam0/seg_00002.idx.jsonl  1 full line + truncated partial line")
    print("  cam0/segments.jsonl       summary for seg_00001 only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
