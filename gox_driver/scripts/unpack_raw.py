#!/usr/bin/env python3
"""Unpack frames from "jai-raw-seg" capture segments into numpy arrays / image files.

Builds on scripts/inspect_raw.py (same directory) for the segment walk; this script adds
PixelFormat decoding. Supported PFNC codes:

    8-bit             Mono8, BayerGR/RG/GB/BG8            -> uint8  (h, w)
    unpacked 10/12/16 Mono10/12/16, BayerXX10/12          -> uint16 (h, w), values LSB-aligned
    GVSP *Packed      Mono10/12Packed, BayerXX10/12Packed -> uint16 (h, w)   (2 px / 3 B, legacy)
    PFNC *p           Mono10p/12p, BayerXX10p/12p         -> uint16 (h, w)   (LSB bit-packed)
    RGB8                                                  -> uint8  (h, w, 3)

Output formats:
    npy    numpy .npy (default; exact values, no dependencies beyond numpy)
    pnm    16-bit PGM / 8-bit PGM/PPM (pure python, viewable in most tools)
    png    via OpenCV when installed (16-bit PNG; with --demosaic: color PNG)

--demosaic converts Bayer mosaics to BGR using OpenCV. Note OpenCV's Bayer constant naming
is offset by one pixel relative to GenICam: GenICam BayerRG (RGGB) uses cv2.COLOR_BayerBG2BGR.

Requires numpy (pip install numpy / apt install python3-numpy). OpenCV optional.
Run --self-test to verify every bit-unpacking routine against hand-computed vectors.
"""

import argparse
import os
import sys

if __package__:
    from . import inspect_raw as ir
else:
    import inspect_raw as ir

try:
    import numpy as np
    if __package__:
        from .jai_raw.pixels import *  # noqa: F401,F403 — preserve the public decoder API
    else:
        from jai_raw.pixels import *  # noqa: F401,F403
except ImportError:
    print("error: this tool requires numpy (pip install numpy)", file=sys.stderr)
    sys.exit(2)

# ----------------------------------------------------------------------------------------------------
# Output writers
# ----------------------------------------------------------------------------------------------------


def write_pnm(path, img):
    """Pure-python PGM (gray, 8/16-bit) or PPM (RGB, 8-bit). 16-bit samples are big-endian per spec."""
    if img.ndim == 2:
        maxval = 255 if img.dtype == np.uint8 else int(img.max()) or 1
        with open(path, "wb") as f:
            f.write(b"P5\n%d %d\n%d\n" % (img.shape[1], img.shape[0], maxval))
            f.write(img.astype(">u2" if maxval > 255 else "u1").tobytes())
    else:
        with open(path, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (img.shape[1], img.shape[0]))
            f.write(img[:, :, ::-1].tobytes() if img.shape[2] == 3 else img.tobytes())


def save(img, path_base, fmt):
    if fmt == "npy":
        np.save(path_base + ".npy", img)
        return path_base + ".npy"
    if fmt == "pnm":
        ext = ".ppm" if img.ndim == 3 else ".pgm"
        write_pnm(path_base + ext, img)
        return path_base + ext
    if fmt == "png":
        import cv2
        cv2.imwrite(path_base + ".png", img)
        return path_base + ".png"
    raise ValueError(fmt)


# ----------------------------------------------------------------------------------------------------
# Frame iteration over segments (reuses inspect_raw)
# ----------------------------------------------------------------------------------------------------


def iter_frames(path):
    """Yields (segment_path, Frame, payload_bytes) for a segment file or a camera/session dir."""
    groups = ir.collect_targets(path)
    if groups is None:
        print("error: no such file or directory: %s" % path, file=sys.stderr)
        sys.exit(2)
    if not groups:
        print("error: no seg_*.raw files found under %s" % path, file=sys.stderr)
        sys.exit(2)
    for d in sorted(groups):
        for seg in sorted(groups[d]):
            f, size = ir.open_segment(seg)
            with f:
                fh = ir.header_or_fallback(f, seg, size, strict=False)
                start = ir.align_up(ir.FILE_HEADER_SIZE, fh.align)
                for ev in ir.iter_records(f, size, start, fh.align):
                    if ev[0] != "frame":
                        continue
                    fr = ev[1]
                    f.seek(fr.off + ir.FRAME_HEADER_SIZE)
                    yield seg, fr, f.read(fr.psz)


# ----------------------------------------------------------------------------------------------------
# Self-test: every unpacker against hand-computed vectors
# ----------------------------------------------------------------------------------------------------


def self_test():
    # GVSP 12Packed: P0=0xABC, P1=0x123 -> b0=0xAB, b1=0x3C (P1 low<<4 | P0 low), b2=0x12
    out = unpack_gvsp12packed(bytes([0xAB, 0x3C, 0x12]))
    assert list(out) == [0xABC, 0x123], out
    # GVSP 10Packed: P0=0x2AB (0b10_10101011), P1=0x155 -> b0=P0>>2=0xAA, b1 = (P1&3)<<4 | (P0&3),
    # P0&3=0b11, P1&3=0b01 -> b1=0x13, b2=P1>>2=0x55
    out = unpack_gvsp10packed(bytes([0xAA, 0x13, 0x55]))
    assert list(out) == [0x2AB, 0x155], out
    # PFNC 12p: P0=0xABC, P1=0x123 -> b0=0xBC, b1=(P1&0xF)<<4 | P0>>8 = 0x3A, b2=P1>>4=0x12
    out = unpack_pfnc12p(bytes([0xBC, 0x3A, 0x12]))
    assert list(out) == [0xABC, 0x123], out
    # PFNC 10p: P0..P3 = 0x001, 0x203, 0x105, 0x3F7 packed LSB-first into 5 bytes.
    p = [0x001, 0x203, 0x105, 0x3F7]
    bits = 0
    for i, v in enumerate(p):
        bits |= v << (10 * i)
    raw = bytes((bits >> (8 * i)) & 0xFF for i in range(5))
    out = unpack_pfnc10p(raw)
    assert list(out) == p, out
    # decode() end-to-end on a 4x2 BayerRG12Packed frame with a known ramp.
    px = np.arange(8, dtype=np.uint16) * 0x123 % 4096
    packed = bytearray()
    for i in range(0, 8, 2):
        p0, p1 = int(px[i]), int(px[i + 1])
        packed += bytes([(p0 >> 4) & 0xFF, ((p1 & 0xF) << 4) | (p0 & 0xF), (p1 >> 4) & 0xFF])
    img, name, pattern = decode(0x010C002B, 4, 2, bytes(packed))
    assert name == "BayerRG12Packed" and pattern == BAYER_RG
    assert img.shape == (2, 4) and (img.flatten() == px).all()
    print("self-test: all unpackers OK")
    return 0


# ----------------------------------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="unpack_raw.py",
        description="Unpack jai-raw-seg frames (Bayer/Mono packed formats) to npy/pnm/png.")
    ap.add_argument("path", nargs="?", help="seg_NNNNN.raw, camera dir, or session dir")
    ap.add_argument("-o", "--out-dir", default="./unpacked", help="output directory (default ./unpacked)")
    ap.add_argument("--format", choices=["npy", "pnm", "png"], default="npy",
                    help="output format (default npy; png requires OpenCV)")
    ap.add_argument("--seq", type=int, action="append",
                    help="unpack only these frame_seq values (repeatable; default: all)")
    ap.add_argument("--every", type=int, default=1, metavar="K", help="keep every K-th frame")
    ap.add_argument("--limit", type=int, default=0, metavar="N", help="stop after N frames")
    ap.add_argument("--demosaic", action="store_true",
                    help="Bayer -> BGR color via OpenCV (output dtype preserved)")
    ap.add_argument("--shift-to-16bit", action="store_true",
                    help="left-shift 10/12-bit values to the full 16-bit range (viewing aid)")
    ap.add_argument("--pad-incomplete", action="store_true",
                    help="zero-fill frames flagged INCOMPLETE and unpack them anyway "
                         "(default: skip them with a warning)")
    ap.add_argument("--self-test", action="store_true", help="verify unpackers and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.path:
        ap.error("path is required (or use --self-test)")

    os.makedirs(args.out_dir, exist_ok=True)
    wanted = set(args.seq) if args.seq else None
    done = 0
    kept = 0
    for seg, fr, payload in iter_frames(args.path):
        if wanted is not None and fr.seq not in wanted:
            continue
        if fr.fl & ir.FRAME_FLAG_INCOMPLETE:
            if not args.pad_incomplete:
                print("warning: skipping seq=%d (INCOMPLETE, %d bytes; --pad-incomplete to keep)"
                      % (fr.seq, fr.psz), file=sys.stderr)
                continue
            need = expected_bytes(fr.pf, fr.w, fr.h)
            if need is not None and len(payload) < need:
                payload = payload + b"\0" * (need - len(payload))
        kept += 1
        if args.every > 1 and (kept - 1) % args.every != 0:
            continue
        try:
            img, name, pattern = decode(fr.pf, fr.w, fr.h, payload)
        except ValueError as e:
            print("error: seq=%d: %s" % (fr.seq, e), file=sys.stderr)
            return 1
        if args.shift_to_16bit and img.dtype == np.uint16:
            img = shift_to_16bit(img, format_for_code(fr.pf))
        if args.demosaic and pattern in CV_BAYER:
            img = demosaic(img, pattern)
        base = os.path.join(args.out_dir, "seq%08d_%s" % (fr.seq, name))
        out_path = save(img, base, args.format)
        print("seq=%d bid=%d %s %dx%d dts=%d -> %s" % (fr.seq, fr.bid, name, fr.w, fr.h, fr.dts,
                                                        out_path))
        done += 1
        if args.limit and done >= args.limit:
            break
    if done == 0:
        print("no frames matched", file=sys.stderr)
        return 1
    print("unpacked %d frame(s) into %s" % (done, os.path.abspath(args.out_dir)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
