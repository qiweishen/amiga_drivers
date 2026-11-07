"""GOX previews: a small 8-bit on-screen image and an on-demand lossless 16-bit PNG.

The native 12-bit Bayer array (unpacked from BayerRG12Packed) remains unchanged. Demosaicing uses uint16 samples.
On-screen previews are derived display products: area-downscaled to at most
``DISPLAY_LONG_EDGE`` pixels and quantized to 8 bits, because browsers and
displays present 8-bit images and the GUI shows them at a few hundred pixels.
The full-resolution 16-bit PNG (DN << 4, preserving each native code exactly)
is produced only when downloaded. Neither product is used for measurement.
"""

from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

from ..ebus import pixels
from .recording import (
    ImageFrame,
    NATIVE_FULL_SCALE,
    frame_from_record,
    snapshot_frame,
    validate_native_image,
)


DISPLAY_LONG_EDGE = 1600


@dataclass(frozen=True)
class SnapshotPreview:
    frame: ImageFrame
    display_png: bytes  # 8-bit, long edge <= DISPLAY_LONG_EDGE
    histogram: list[int]
    clipped_pct: float
    mean_dn: float


def _png_bytes(rgb: np.ndarray, depth: int) -> bytes:
    ok, encoded = cv2.imencode(".png", cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))
    if not ok:
        raise ValueError(f"{depth}-bit PNG encoding failed")
    # The codec byte stream is uint8; it is not an 8-bit pixel image.
    data = encoded.tobytes()
    if (len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR"
            or data[24] != depth or data[25] != 2):
        raise ValueError(f"PNG encoder did not preserve {depth}-bit RGB samples")
    return data


def png16_bytes(rgb16: np.ndarray) -> bytes:
    """Encode uint16 RGB samples unchanged; no scaling or 8-bit conversion."""
    if rgb16.dtype != np.uint16 or rgb16.ndim != 3 or rgb16.shape[2] != 3 or rgb16.size == 0:
        raise ValueError("GOX PNG encoding requires a nonempty uint16 RGB image")
    return _png_bytes(rgb16, 16)


def png8_bytes(rgb8: np.ndarray) -> bytes:
    if rgb8.dtype != np.uint8 or rgb8.ndim != 3 or rgb8.shape[2] != 3 or rgb8.size == 0:
        raise ValueError("Display PNG encoding requires a nonempty uint8 RGB image")
    return _png_bytes(rgb8, 8)


def display_scale(shape: tuple[int, ...], long_edge: int = DISPLAY_LONG_EDGE) -> float:
    return min(1.0, long_edge / max(shape[0], shape[1]))


def downscale(image: np.ndarray, long_edge: int = DISPLAY_LONG_EDGE) -> np.ndarray:
    """Area-average to at most ``long_edge`` pixels; a new array either way."""
    scale = display_scale(image.shape, long_edge)
    if scale >= 1.0:
        return image.copy()
    height, width = image.shape[:2]
    size = (max(1, round(width * scale)), max(1, round(height * scale)))
    return cv2.resize(image, size, interpolation=cv2.INTER_AREA)


def display_points(points: np.ndarray, native_shape: tuple[int, ...], display_shape: tuple[int, ...]) -> np.ndarray:
    """Map native pixel coordinates onto a resized image (pixel-center convention)."""
    ratios = np.array([display_shape[1] / native_shape[1], display_shape[0] / native_shape[0]])
    return (np.asarray(points, dtype=np.float64) + 0.5) * ratios - 0.5


def _check_native_rgb(rgb_dn: np.ndarray) -> None:
    if (rgb_dn.dtype != np.uint16 or rgb_dn.ndim != 3 or rgb_dn.shape[2] != 3
            or rgb_dn.size == 0 or int(rgb_dn.max()) > NATIVE_FULL_SCALE):
        raise ValueError("Native GOX preview requires uint16 RGB in DN 0..4095")


def native_rgb(frame: ImageFrame) -> np.ndarray:
    """Demosaic the unpacked 12-bit Bayer image into full-resolution uint16 RGB, still in native DN."""
    validate_native_image(frame.image, frame.pixel_format)
    bgr = pixels.demosaic(frame.image, pixels.BAYER_RG)
    return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)


def native_display_rgb8(rgb_dn: np.ndarray, long_edge: int = DISPLAY_LONG_EDGE) -> np.ndarray:
    """Linear DN / 4095 at display size, quantized to 8 bits for the screen."""
    _check_native_rgb(rgb_dn)
    small = downscale(rgb_dn, long_edge).astype(np.float32)
    return np.rint(np.clip(small / NATIVE_FULL_SCALE, 0.0, 1.0) * 255.0).astype(np.uint8)


def native_display_png(rgb_dn: np.ndarray, long_edge: int = DISPLAY_LONG_EDGE) -> bytes:
    return png8_bytes(native_display_rgb8(rgb_dn, long_edge))


def native_full_png16(rgb_dn: np.ndarray) -> bytes:
    """Map each 12-bit DN to an exact 16-bit code, DN * 16, at full resolution."""
    _check_native_rgb(rgb_dn)
    return png16_bytes(np.left_shift(rgb_dn, 4))


def snapshot(directory: Path) -> SnapshotPreview:
    frame = snapshot_frame(directory)
    rgb_dn = native_rgb(frame)
    # Report original photosite measurements rather than interpolated RGB or
    # grayscale values; saturated red/blue sites must remain visible in metrics.
    histogram, _ = np.histogram(frame.image, bins=64, range=(0, NATIVE_FULL_SCALE + 1))
    clipped = float(np.count_nonzero(frame.image == NATIVE_FULL_SCALE) / frame.image.size * 100.0)
    return SnapshotPreview(frame, native_display_png(rgb_dn), [int(value) for value in histogram],
                           clipped, float(frame.image.mean()))


def snapshot_full_png16(directory: Path) -> bytes:
    """Re-decode the snapshot's first frame for the full-resolution download."""
    return native_full_png16(native_rgb(snapshot_frame(directory)))


def live_display_png(frame: ImageFrame) -> bytes:
    """8-bit screen preview of one recorded frame (Data Live)."""
    return native_display_png(native_rgb(frame))


def record_full_png16(record) -> bytes:
    """Re-decode one committed record for the full-resolution download."""
    return native_full_png16(native_rgb(frame_from_record(record)))
