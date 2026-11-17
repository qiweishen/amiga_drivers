"""Native DN decoding; raw recording bytes are never modified."""

from __future__ import annotations

import numpy as np

from .pfnc import BAYER_RG, format_for_code, row_bytes


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


_UNPACK = {
    "u8": lambda buf: np.frombuffer(buf, np.uint8),
    "u16": lambda buf: np.frombuffer(buf, "<u2"),
    "gvsp10p": unpack_gvsp10packed,
    "gvsp12p": unpack_gvsp12packed,
}


def decode(pf_code, width, height, payload):
    """Decode only a complete, tightly packed image; ambiguous layout is an error."""
    pixel_format = format_for_code(pf_code)
    if height <= 0:
        raise ValueError("image dimensions must be positive")
    need = row_bytes(pixel_format, width) * height
    if len(payload) < need:
        raise ValueError(f"payload too short for {pixel_format.name} {width}x{height}: "
                         f"{len(payload)} < {need} bytes (incomplete frame)")
    if len(payload) != need:
        raise ValueError("payload has padding/chunks or an unknown layout; refusing to discard bytes or guess stride")
    image = _UNPACK[pixel_format.layout](payload).reshape(height, width)
    return image, pixel_format.name, pixel_format.pattern


def _image_layout(record, payload):
    """Validate the SDK-described image slice: (pf, width, height, offset, row_bytes, stride).

    X padding is bytes per row; Y padding is trailing bytes. Pixel/ROI offsets
    are metadata, not byte offsets.
    """
    if not record.valid_frame:
        raise ValueError("Recorded buffer is not a valid image frame")
    image = record.metadata["image"]
    offset = image["payload_offset"]
    if offset is None:
        raise ValueError("SDK image byte offset is unknown; refusing to guess")
    pf, width, height = image["pixel_format"], image["width"], image["height"]
    if height <= 0:
        raise ValueError("Unsupported SDK image geometry")
    row = row_bytes(format_for_code(pf), width)
    stride = row + image["padding_x"]
    expected = stride * height + image["padding_y"]
    if (image["effective_image_size"] != row * height or image["image_size"] != expected
            or offset + expected > len(payload) or len(payload) != record.payload_size):
        raise ValueError("SDK image layout disagrees with the recorded payload")
    return pf, width, height, offset, row, stride


def image_rows(record, payload):
    """The declared image's packed rows as a read-only (height, row_bytes) uint8 view.

    Same layout checks as ``decode_record``; X padding, chunks and other bytes
    outside the image are excluded. Nothing is copied or decoded, so a caller can
    unpack only the rows and columns it needs.
    """
    _, _, height, offset, row, stride = _image_layout(record, payload)
    rows = np.frombuffer(payload, np.uint8, count=stride * height, offset=offset).reshape(height, stride)
    return rows[:, :row]


def decode_record(record, payload):
    """Derive a tight image from SDK-described bytes, retaining the raw buffer.

    Chunk and other leading/trailing bytes stay in the original payload; only
    the declared image slice is decoded here.
    """
    pf, width, height, offset, row, stride = _image_layout(record, payload)
    if stride == row:
        tight = memoryview(payload)[offset:offset + row * height]
    else:
        tight = b"".join(payload[offset + index * stride:offset + index * stride + row] for index in range(height))
    return decode(pf, width, height, tight)


def demosaic(img, pattern):
    """OpenCV bilinear demosaic of a BayerRG frame to BGR, keeping the sample depth."""
    import cv2  # optional dependency, imported on demand
    if pattern != BAYER_RG:
        raise ValueError(f"Demosaicing needs a BayerRG frame (got pattern {pattern})")
    return cv2.cvtColor(img, cv2.COLOR_BayerRGGB2BGR)
