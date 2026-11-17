"""SDK-free PFNC names and sample layouts of the formats Phenomate records.

GOX records BayerRG12Packed (focus assist streams BayerRG8); FX10 records one of
the Mono formats. Any other PixelFormat is rejected rather than guessed.
"""

from typing import NamedTuple


class PixelFormat(NamedTuple):
    name: str
    layout: str
    pattern: str
    bits: int

    @property
    def full_scale(self) -> int:
        return (1 << self.bits) - 1


MONO, BAYER_RG = "Mono", "RG"

PFNC = {
    0x01080001: PixelFormat("Mono8", "u8", MONO, 8),
    0x01100003: PixelFormat("Mono10", "u16", MONO, 10),
    0x01100005: PixelFormat("Mono12", "u16", MONO, 12),
    # GigE Vision "Packed": 2 px / 3 B, high bits in the outer bytes.
    0x010C0004: PixelFormat("Mono10Packed", "gvsp10p", MONO, 10),
    0x010C0006: PixelFormat("Mono12Packed", "gvsp12p", MONO, 12),
    0x01080009: PixelFormat("BayerRG8", "u8", BAYER_RG, 8),
    0x010C002B: PixelFormat("BayerRG12Packed", "gvsp12p", BAYER_RG, 12),
}

# Wire layout -> (pixels, bytes) of one packing group.
GROUPS = {"u8": (1, 1), "u16": (1, 2), "gvsp10p": (2, 3), "gvsp12p": (2, 3)}


def format_for_code(code: int) -> PixelFormat:
    try:
        return PFNC[code]
    except KeyError:
        raise ValueError(f"Unsupported PixelFormat 0x{code:08X}") from None


def row_bytes(pixel_format: PixelFormat, width: int) -> int:
    """Bytes of one tightly packed row; the width must be a whole number of packing groups."""
    pixels, size = GROUPS[pixel_format.layout]
    if width <= 0 or width % pixels:
        raise ValueError(f"{pixel_format.name} row width {width} is not a whole number of packing groups")
    return width // pixels * size
