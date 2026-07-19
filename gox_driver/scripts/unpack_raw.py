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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inspect_raw as ir  # noqa: E402  (segment walk + header parsing)

try:
    import numpy as np
except ImportError:
    print("error: this tool requires numpy (pip install numpy)", file=sys.stderr)
    sys.exit(2)

# ----------------------------------------------------------------------------------------------------
# PFNC pixel format registry
# ----------------------------------------------------------------------------------------------------

MONO, BAYER_GR, BAYER_RG, BAYER_GB, BAYER_BG, RGB = "Mono", "GR", "RG", "GB", "BG", "RGB"

# code: (name, layout, pattern)  where layout selects the decoder below.
PFNC = {
    0x01080001: ("Mono8", "u8", MONO),
    0x01080008: ("BayerGR8", "u8", BAYER_GR),
    0x01080009: ("BayerRG8", "u8", BAYER_RG),
    0x0108000A: ("BayerGB8", "u8", BAYER_GB),
    0x0108000B: ("BayerBG8", "u8", BAYER_BG),
    0x01100003: ("Mono10", "u16", MONO),
    0x01100005: ("Mono12", "u16", MONO),
    0x01100007: ("Mono16", "u16", MONO),
    0x0110000C: ("BayerGR10", "u16", BAYER_GR),
    0x0110000D: ("BayerRG10", "u16", BAYER_RG),
    0x0110000E: ("BayerGB10", "u16", BAYER_GB),
    0x0110000F: ("BayerBG10", "u16", BAYER_BG),
    0x01100010: ("BayerGR12", "u16", BAYER_GR),
    0x01100011: ("BayerRG12", "u16", BAYER_RG),
    0x01100012: ("BayerGB12", "u16", BAYER_GB),
    0x01100013: ("BayerBG12", "u16", BAYER_BG),
    # GigE Vision legacy "Packed": 2 px / 3 B, high bits in the outer bytes.
    0x010C0004: ("Mono10Packed", "gvsp10p", MONO),
    0x010C0006: ("Mono12Packed", "gvsp12p", MONO),
    0x010C0026: ("BayerGR10Packed", "gvsp10p", BAYER_GR),
    0x010C0027: ("BayerRG10Packed", "gvsp10p", BAYER_RG),
    0x010C0028: ("BayerGB10Packed", "gvsp10p", BAYER_GB),
    0x010C0029: ("BayerBG10Packed", "gvsp10p", BAYER_BG),
    0x010C002A: ("BayerGR12Packed", "gvsp12p", BAYER_GR),
    0x010C002B: ("BayerRG12Packed", "gvsp12p", BAYER_RG),
    0x010C002C: ("BayerGB12Packed", "gvsp12p", BAYER_GB),
    0x010C002D: ("BayerBG12Packed", "gvsp12p", BAYER_BG),
    # PFNC lsb-packed "p" variants (5GigE-generation cameras).
    0x010A0046: ("Mono10p", "pfnc10p", MONO),
    0x010C0047: ("Mono12p", "pfnc12p", MONO),
    0x010A0052: ("BayerBG10p", "pfnc10p", BAYER_BG),
    0x010A0054: ("BayerGB10p", "pfnc10p", BAYER_GB),
    0x010A0056: ("BayerGR10p", "pfnc10p", BAYER_GR),
    0x010A0058: ("BayerRG10p", "pfnc10p", BAYER_RG),
    0x010C0053: ("BayerBG12p", "pfnc12p", BAYER_BG),
    0x010C0055: ("BayerGB12p", "pfnc12p", BAYER_GB),
    0x010C0057: ("BayerGR12p", "pfnc12p", BAYER_GR),
    0x010C0059: ("BayerRG12p", "pfnc12p", BAYER_RG),
    0x02180014: ("RGB8", "rgb8", RGB),
}


def unpack_gvsp12packed(buf):
    """GVSP 12Packed: b0=P0[11:4], b1 = P1[3:0]<<4 | P0[3:0], b2=P1[11:4]."""
    b = np.frombuffer(buf, np.uint8).reshape(-1, 3).astype(np.uint16)
    out = np.empty(b.shape[0] * 2, np.uint16)
    out[0::2] = (b[:, 0] << 4) | (b[:, 1] & 0x0F)
    out[1::2] = (b[:, 2] << 4) | (b[:, 1] >> 4)
    return out


def unpack_gvsp10packed(buf):
    """GVSP 10Packed: b0=P0[9:2], b1 bits[1:0]=P0[1:0] bits[5:4]=P1[1:0], b2=P1[9:2]."""
    b = np.frombuffer(buf, np.uint8).reshape(-1, 3).astype(np.uint16)
    out = np.empty(b.shape[0] * 2, np.uint16)
    out[0::2] = (b[:, 0] << 2) | (b[:, 1] & 0x03)
    out[1::2] = (b[:, 2] << 2) | ((b[:, 1] >> 4) & 0x03)
    return out


def unpack_pfnc12p(buf):
    """PFNC 12p (LSB-packed): p0 = b0 | (b1&0x0F)<<8, p1 = b1>>4 | b2<<4."""
    b = np.frombuffer(buf, np.uint8).reshape(-1, 3).astype(np.uint16)
    out = np.empty(b.shape[0] * 2, np.uint16)
    out[0::2] = b[:, 0] | ((b[:, 1] & 0x0F) << 8)
    out[1::2] = (b[:, 1] >> 4) | (b[:, 2] << 4)
    return out


def unpack_pfnc10p(buf):
    """PFNC 10p (LSB-packed): 4 px / 5 B continuous little-endian bit stream."""
    b = np.frombuffer(buf, np.uint8).reshape(-1, 5).astype(np.uint16)
    out = np.empty(b.shape[0] * 4, np.uint16)
    out[0::4] = b[:, 0] | ((b[:, 1] & 0x03) << 8)
    out[1::4] = (b[:, 1] >> 2) | ((b[:, 2] & 0x0F) << 6)
    out[2::4] = (b[:, 2] >> 4) | ((b[:, 3] & 0x3F) << 4)
    out[3::4] = (b[:, 3] >> 6) | (b[:, 4] << 2)
    return out


NEED_BYTES = {
    "u8": lambda n: n,
    "u16": lambda n: n * 2,
    "rgb8": lambda n: n * 3,
    "gvsp12p": lambda n: (n + 1) // 2 * 3,
    "gvsp10p": lambda n: (n + 1) // 2 * 3,
    "pfnc12p": lambda n: (n + 1) // 2 * 3,
    "pfnc10p": lambda n: (n + 3) // 4 * 5,
}


def expected_bytes(pf_code, width, height):
    """Full-frame payload size for a known PFNC code, or None."""
    entry = PFNC.get(pf_code)
    return None if entry is None else NEED_BYTES[entry[1]](width * height)


def decode(pf_code, width, height, payload):
    """payload bytes -> numpy array. Raises ValueError on unknown/short input."""
    entry = PFNC.get(pf_code)
    if entry is None:
        raise ValueError("unsupported PixelFormat 0x%08X (extend the PFNC table)" % pf_code)
    name, layout, pattern = entry
    n = width * height
    need = NEED_BYTES[layout](n)
    if len(payload) < need:
        raise ValueError("payload too short for %s %dx%d: %d < %d bytes (INCOMPLETE frame? "
                         "use --skip-incomplete)" % (name, width, height, len(payload), need))
    buf = payload[:need]
    if layout == "u8":
        arr = np.frombuffer(buf, np.uint8)
    elif layout == "u16":
        arr = np.frombuffer(buf, "<u2")
    elif layout == "rgb8":
        return np.frombuffer(buf, np.uint8).reshape(height, width, 3), name, pattern
    elif layout == "gvsp12p":
        arr = unpack_gvsp12packed(buf)[:n]
    elif layout == "gvsp10p":
        arr = unpack_gvsp10packed(buf)[:n]
    elif layout == "pfnc12p":
        arr = unpack_pfnc12p(buf)[:n]
    else: # pfnc10p
        arr = unpack_pfnc10p(buf)[:n]
    return arr.reshape(height, width), name, pattern


# GenICam Bayer pattern -> OpenCV constant name (OpenCV names are offset by one pixel).
CV_BAYER = {BAYER_RG: "COLOR_BayerBG2BGR", BAYER_GR: "COLOR_BayerGB2BGR",
            BAYER_GB: "COLOR_BayerGR2BGR", BAYER_BG: "COLOR_BayerRG2BGR"}


def demosaic(img, pattern):
    import cv2  # optional dependency, imported on demand
    if pattern not in CV_BAYER:
        raise ValueError("--demosaic needs a Bayer frame (got pattern %s)" % pattern)
    return cv2.cvtColor(img, getattr(cv2, CV_BAYER[pattern]))


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
            bpp = (fr.pf >> 16) & 0xFF
            depth = 12 if "12" in name else (10 if "10" in name else bpp)
            img = (img << (16 - depth)).astype(np.uint16)
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
