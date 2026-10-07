"""SDK-free PFNC names, storage layouts and effective value depths."""

from typing import NamedTuple


class PixelFormat(NamedTuple):
    name: str
    layout: str
    pattern: str
    bits: int

    @property
    def full_scale(self) -> int:
        return (1 << self.bits) - 1


MONO, BAYER_GR, BAYER_RG, BAYER_GB, BAYER_BG, RGB = "Mono", "GR", "RG", "GB", "BG", "RGB"

# One registry: name, wire layout, Bayer pattern, and effective sample bits.
PFNC = {
    0x01080001: PixelFormat("Mono8", "u8", MONO, 8),
    0x01080008: PixelFormat("BayerGR8", "u8", BAYER_GR, 8),
    0x01080009: PixelFormat("BayerRG8", "u8", BAYER_RG, 8),
    0x0108000A: PixelFormat("BayerGB8", "u8", BAYER_GB, 8),
    0x0108000B: PixelFormat("BayerBG8", "u8", BAYER_BG, 8),
    0x01100003: PixelFormat("Mono10", "u16", MONO, 10),
    0x01100005: PixelFormat("Mono12", "u16", MONO, 12),
    0x01100007: PixelFormat("Mono16", "u16", MONO, 16),
    0x0110000C: PixelFormat("BayerGR10", "u16", BAYER_GR, 10),
    0x0110000D: PixelFormat("BayerRG10", "u16", BAYER_RG, 10),
    0x0110000E: PixelFormat("BayerGB10", "u16", BAYER_GB, 10),
    0x0110000F: PixelFormat("BayerBG10", "u16", BAYER_BG, 10),
    0x01100010: PixelFormat("BayerGR12", "u16", BAYER_GR, 12),
    0x01100011: PixelFormat("BayerRG12", "u16", BAYER_RG, 12),
    0x01100012: PixelFormat("BayerGB12", "u16", BAYER_GB, 12),
    0x01100013: PixelFormat("BayerBG12", "u16", BAYER_BG, 12),
    # GigE Vision legacy "Packed": 2 px / 3 B, high bits in the outer bytes.
    0x010C0004: PixelFormat("Mono10Packed", "gvsp10p", MONO, 10),
    0x010C0006: PixelFormat("Mono12Packed", "gvsp12p", MONO, 12),
    0x010C0026: PixelFormat("BayerGR10Packed", "gvsp10p", BAYER_GR, 10),
    0x010C0027: PixelFormat("BayerRG10Packed", "gvsp10p", BAYER_RG, 10),
    0x010C0028: PixelFormat("BayerGB10Packed", "gvsp10p", BAYER_GB, 10),
    0x010C0029: PixelFormat("BayerBG10Packed", "gvsp10p", BAYER_BG, 10),
    0x010C002A: PixelFormat("BayerGR12Packed", "gvsp12p", BAYER_GR, 12),
    0x010C002B: PixelFormat("BayerRG12Packed", "gvsp12p", BAYER_RG, 12),
    0x010C002C: PixelFormat("BayerGB12Packed", "gvsp12p", BAYER_GB, 12),
    0x010C002D: PixelFormat("BayerBG12Packed", "gvsp12p", BAYER_BG, 12),
    # PFNC lsb-packed "p" variants (5GigE-generation cameras).
    0x010A0046: PixelFormat("Mono10p", "pfnc10p", MONO, 10),
    0x010C0047: PixelFormat("Mono12p", "pfnc12p", MONO, 12),
    0x010A0052: PixelFormat("BayerBG10p", "pfnc10p", BAYER_BG, 10),
    0x010A0054: PixelFormat("BayerGB10p", "pfnc10p", BAYER_GB, 10),
    0x010A0056: PixelFormat("BayerGR10p", "pfnc10p", BAYER_GR, 10),
    0x010A0058: PixelFormat("BayerRG10p", "pfnc10p", BAYER_RG, 10),
    0x010C0053: PixelFormat("BayerBG12p", "pfnc12p", BAYER_BG, 12),
    0x010C0055: PixelFormat("BayerGB12p", "pfnc12p", BAYER_GB, 12),
    0x010C0057: PixelFormat("BayerGR12p", "pfnc12p", BAYER_GR, 12),
    0x010C0059: PixelFormat("BayerRG12p", "pfnc12p", BAYER_RG, 12),
    0x02180014: PixelFormat("RGB8", "rgb8", RGB, 8),
}


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


_BY_NAME = {value.name: value for value in PFNC.values()}


def format_for_code(code: int) -> PixelFormat:
    try:
        return PFNC[code]
    except KeyError:
        raise ValueError(f"Unsupported PixelFormat 0x{code:08X}") from None


def format_for_name(name: str) -> PixelFormat:
    try:
        return _BY_NAME[name]
    except KeyError:
        raise ValueError(f"Unknown decoded PixelFormat: {name}") from None

def nominal_payload_bytes(code: int, width: int, height: int) -> int:
    """Known packing first; preserve the inspector's PFNC-bit fallback for unknown formats."""
    known = expected_bytes(code, width, height)
    return known if known is not None else (width * height * ((code >> 16) & 0xFF) + 7) // 8
