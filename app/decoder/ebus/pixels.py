"""Native DN decoding; raw recording bytes are never modified."""

from __future__ import annotations

import numpy as np

from .pfnc import (BAYER_GR, BAYER_RG, BAYER_GB, BAYER_BG, RGB, PFNC, NEED_BYTES,
                   PixelFormat, expected_bytes, format_for_code, format_for_name)


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


def decode(pf_code, width, height, payload):
    """Decode only a complete, tightly packed image; ambiguous layout is an error."""
    entry = PFNC.get(pf_code)
    if entry is None:
        raise ValueError("unsupported PixelFormat 0x%08X (extend the PFNC table)" % pf_code)
    name, layout, pattern, _ = entry
    if width <= 0 or height <= 0:
        raise ValueError("image dimensions must be positive")
    group = 4 if layout == "pfnc10p" else 2 if layout in ("gvsp12p", "gvsp10p", "pfnc12p") else 1
    if width % group:
        raise ValueError("packed row width is not a complete packing group; row layout must be supplied explicitly")
    n = width * height
    need = NEED_BYTES[layout](n)
    if len(payload) < need:
        raise ValueError("payload too short for %s %dx%d: %d < %d bytes (incomplete frame)" %
                         (name, width, height, len(payload), need))
    if len(payload) != need:
        raise ValueError("payload has padding/chunks or an unknown layout; refusing to discard bytes or guess stride")
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
    else:  # pfnc10p
        arr = unpack_pfnc10p(buf)[:n]
    return arr.reshape(height, width), name, pattern


def decode_record(record, payload):
    """Derive a tight image from SDK-described bytes, retaining the raw buffer.

    X padding is bytes per row; Y padding is trailing bytes. Pixel/ROI offsets
    are metadata, not byte offsets. Chunk and other leading/trailing bytes stay
    in the original payload; only the declared image slice is decoded here.
    """
    if not record.valid_frame:
        raise ValueError("Recorded buffer is not a valid image frame")
    image = record.metadata["image"]
    offset = image["payload_offset"]
    if offset is None:
        raise ValueError("SDK image byte offset is unknown; refusing to guess")
    pf, width, height = image["pixel_format"], image["width"], image["height"]
    pixel_format = format_for_code(pf)
    group = 4 if pixel_format.layout == "pfnc10p" else 2 if pixel_format.layout in ("gvsp12p", "gvsp10p",
                                                                                    "pfnc12p") else 1
    if width <= 0 or height <= 0 or width % group:
        raise ValueError("Unsupported SDK image geometry or packed row width")
    row_bytes = expected_bytes(pf, width, 1)
    stride = row_bytes + image["padding_x"]
    expected = stride * height + image["padding_y"]
    if (image["effective_image_size"] != row_bytes * height or image["image_size"] != expected
            or offset + expected > len(payload) or len(payload) != record.payload_size):
        raise ValueError("SDK image layout disagrees with the recorded payload")
    if row_bytes * height > 128 * 1024 * 1024:
        raise ValueError("Decoded image exceeds the memory budget")
    if image["padding_x"] == 0:
        tight = memoryview(payload)[offset:offset + row_bytes * height]
    else:
        tight = b"".join(payload[offset + row * stride:offset + row * stride + row_bytes] for row in range(height))
    return decode(pf, width, height, tight)


# GenICam Bayer pattern -> OpenCV constant (pattern named by the top-left 2x2 cell).
CV_BAYER = {BAYER_RG: "COLOR_BayerRGGB2BGR", BAYER_GR: "COLOR_BayerGRBG2BGR",
            BAYER_GB: "COLOR_BayerGBRG2BGR", BAYER_BG: "COLOR_BayerBGGR2BGR"}


def demosaic(img, pattern):
    import cv2  # optional dependency, imported on demand
    if pattern not in CV_BAYER:
        raise ValueError("Demosaicing needs a Bayer frame (got pattern %s)" % pattern)
    return cv2.cvtColor(img, getattr(cv2, CV_BAYER[pattern]))

