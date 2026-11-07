"""Read native GOX images from ebus-raw files, without device or GUI state."""

from dataclasses import dataclass
from pathlib import Path

import numpy as np

from ..ebus import pixels, recording


BAYER_RG12_PACKED_CODE = 0x010C002B  # GVSP BayerRG12Packed: 2 px / 3 B
NATIVE_PIXEL_FORMAT = "BayerRG12Packed"
NATIVE_FULL_SCALE = 4095


@dataclass(frozen=True)
class ImageFrame:
    record: recording.Record
    image: np.ndarray
    pixel_format: str
    skipped_invalid: bool = False


def _camera(directory: Path) -> Path:
    directory = Path(directory)
    if recording.device_metadata(directory)["kind"] != "gox":
        raise ValueError("Recording is not a GOX ebus-raw camera profile")
    return directory


def validate_native_image(image: np.ndarray, pixel_format: str) -> None:
    """Require the unpacked native BayerRG12Packed photosites, with no display conversion."""
    if pixel_format != NATIVE_PIXEL_FORMAT:
        raise ValueError(f"GOX requires {NATIVE_PIXEL_FORMAT}; received {pixel_format}")
    if image.dtype != np.uint16 or image.ndim != 2 or image.size == 0:
        raise ValueError(f"{NATIVE_PIXEL_FORMAT} requires a nonempty uint16 photosite image")
    if int(image.max()) > NATIVE_FULL_SCALE:
        raise ValueError(f"{NATIVE_PIXEL_FORMAT} sample exceeds the native 12-bit DN range")


def _decode_native(record: recording.Record, payload: bytes, skipped_invalid: bool = False) -> ImageFrame:
    # Reject other camera modes before decoding, so an 8-bit image is never
    # produced even temporarily by this GOX path.
    if record.pf != BAYER_RG12_PACKED_CODE:
        raise ValueError(f"GOX requires {NATIVE_PIXEL_FORMAT}; received PFNC 0x{record.pf:08X}")
    image, name, _ = pixels.decode_record(record, payload)
    validate_native_image(image, name)
    return ImageFrame(record, image, name, skipped_invalid)


def snapshot_frame(directory: Path, *, verify_crc: bool = False) -> ImageFrame:
    record, payload, skipped = recording.first_preview_frame(_camera(directory), verify_crc=verify_crc)
    return _decode_native(record, payload, skipped)


def frame_from_record(record: recording.Record) -> ImageFrame:
    """Re-decode one previously selected committed record, e.g. for a download."""
    return _decode_native(record, recording.read_payload(record))


def latest_frame(directory: Path) -> ImageFrame:
    frames = recording.latest_records(_camera(directory), 1)
    if not frames:
        raise ValueError("No complete valid raw image yet")
    record = frames[-1]
    return _decode_native(record, recording.read_payload(record))
