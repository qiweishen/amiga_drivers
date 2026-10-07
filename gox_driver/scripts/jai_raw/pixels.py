"""Native DN decoding shared by GoX CLI and app previews."""

from __future__ import annotations

import numpy as np

from .pfnc import (MONO, BAYER_GR, BAYER_RG, BAYER_GB, BAYER_BG, RGB, PFNC, NEED_BYTES,
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
        raise ValueError("payload too short for %s %dx%d: %d < %d bytes (INCOMPLETE frame? "
                         "use --skip-incomplete)" % (name, width, height, len(payload), need))
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

def shift_to_16bit(image: np.ndarray, pixel_format: PixelFormat) -> np.ndarray:
    return (image << (16 - pixel_format.bits)).astype(np.uint16) if image.dtype == np.uint16 else image


def prepare_snapshot_image(image: np.ndarray, name: str) -> tuple[np.ndarray, float]:
    """Measure native photosite/component saturation before display transforms."""
    pixel_format = format_for_name(name)
    shape_ok = image.ndim == 3 and image.shape[2] == 3 if pixel_format.pattern == RGB else image.ndim == 2
    expected_dtype = np.uint8 if pixel_format.bits == 8 else np.uint16
    if not shape_ok or image.size == 0 or image.dtype != expected_dtype:
        raise ValueError(f"Decoded image shape/dtype does not match {name}")
    if int(image.max()) > pixel_format.full_scale:
        raise ValueError(f"Decoded sample exceeds {pixel_format.bits}-bit full scale")
    clipped = float(np.count_nonzero(image == pixel_format.full_scale) / image.size * 100.0)
    preview = shift_to_16bit(image, pixel_format)
    if pixel_format.pattern in CV_BAYER:
        preview = demosaic(preview, pixel_format.pattern)
    return preview, clipped


def live_image(image: np.ndarray, pixel_format: PixelFormat) -> np.ndarray:
    """Existing live tone mapping, using the same explicit effective bit depth."""
    if pixel_format.pattern in CV_BAYER:
        image = demosaic(image, pixel_format.pattern)
    return np.clip(image.astype(np.float32) / pixel_format.full_scale * 255.0, 0, 255).astype(np.uint8)
