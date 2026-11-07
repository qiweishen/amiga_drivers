"""FX10 profile and image readers for ebus-raw recordings.

Snapshots and references require finalized sessions. Live reads select a bounded
suffix of complete valid records; they do not audit the earlier recording.
Decoded DN retain their native values. No reader changes the source files.
"""

from __future__ import annotations

import math
import numpy as np
from dataclasses import dataclass
from pathlib import Path

from app.decoder.ebus import pixels as raw_pixels, recording as raw_recording

MAX_LINE_BYTES = 64 * 1024 * 1024
_UINT64_MAX = (1 << 64) - 1
_PIXELS = {"Mono8": (1, 255), "Mono10": (2, 1023), "Mono10Packed": (2, 1023),
           "Mono12": (2, 4095), "Mono12Packed": (2, 4095)}


def _uint(value: object, name: str, *, positive: bool = False) -> int:
    if type(value) is not int or not (int(positive) <= value <= _UINT64_MAX):
        raise ValueError(f"Invalid {name}")
    return value


def _axis(values: object, bands: int) -> tuple[float, ...]:
    if not isinstance(values, (list, tuple)):
        raise ValueError("Invalid wavelength axis")
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) for v in values):
        raise ValueError("Invalid wavelength axis")
    out = tuple(float(v) for v in values)
    if out and (len(out) != bands or not all(math.isfinite(v) and v > 0 for v in out)):
        raise ValueError("Invalid wavelength axis")
    return out


@dataclass(frozen=True)
class Capture:
    samples: int
    bands: int
    bytes_per_pixel: int
    full_scale: int
    pixel_format: str
    wavelengths_nm: tuple[float, ...]
    expected_frame_rate_hz: float

    @property
    def line_bytes(self) -> int:
        """Decoded image size, independent of raw packing, padding and chunks."""
        return self.samples * self.bands * self.bytes_per_pixel


def load_capture(directory: Path) -> Capture:
    if not raw_recording.is_session(directory):
        raise ValueError("FX10 preview supports only ebus-raw recordings")
    device = raw_recording.device_metadata(directory)
    if device["kind"] != "fx10" or not isinstance(device.get("profile"), dict):
        raise ValueError("Recording is not an FX10 camera profile")
    doc = device["profile"]
    pixel_format = doc.get("pixel_format")
    if not isinstance(pixel_format, str) or pixel_format not in _PIXELS:
        raise ValueError("Unsupported FX10 PixelFormat")
    bpp, expected_scale = _PIXELS[pixel_format]
    samples = _uint(doc["samples"], "capture samples", positive=True)
    bands = _uint(doc["bands"], "capture bands", positive=True)
    full_scale = _uint(doc["full_scale"], "capture full_scale", positive=True)
    if full_scale != expected_scale:
        raise ValueError("Capture PixelFormat and full scale disagree")
    if samples > 65535 or bands > 65535 or samples * bands * bpp > MAX_LINE_BYTES:
        raise ValueError("Capture geometry exceeds the preview memory budget")
    rate = doc["expected_frame_rate_hz"]
    if isinstance(rate, bool) or not isinstance(rate, (int, float)) or not math.isfinite(
            rate) or not 0 <= rate <= 100000:
        raise ValueError("Invalid capture frame rate")
    return Capture(samples, bands, bpp, full_scale, pixel_format,
                   _axis(doc.get("wavelengths_nm", []), bands), float(rate))


def _decode_raw(record, capture: Capture):
    image, name, _ = raw_pixels.decode_record(record, raw_recording.read_payload(record))
    if name != capture.pixel_format or image.shape != (capture.bands, capture.samples):
        raise ValueError("Recorded image disagrees with the FX10 profile")
    # Detach views from acquired buffers so statistics do not retain chunk tails.
    return image.copy()


def raw_frame_batches(directory: Path, capture: Capture, *, batch_bytes=8 * 1024 * 1024):
    batch = []
    for record in raw_recording.session_records(directory):
        if not record.valid_frame:
            continue
        batch.append(_decode_raw(record, capture))
        if len(batch) * capture.line_bytes >= batch_bytes:
            yield np.stack(batch)
            batch.clear()
    if batch:
        yield np.stack(batch)


def validate_finalized(directory: Path) -> None:
    """Validate metadata, finalization and counts without loading pixel bytes."""
    load_capture(directory)
    if not sum(record.valid_frame for record in raw_recording.session_records(directory)):
        raise ValueError("Finalized FX10 recording has no valid frames")


def raw_tail_cube(directory: Path, capture: Capture, limit: int):
    limit = min(max(1, limit), max(1, MAX_LINE_BYTES // capture.line_bytes))
    records = raw_recording.latest_records(directory, limit)
    if not records:
        raise ValueError("No complete valid raw image yet")
    return np.stack([_decode_raw(record, capture) for record in records]), records[-1].hrt


def snapshot_cube(directory: Path) -> tuple[Capture, np.ndarray]:
    capture = load_capture(directory)
    chunks, count = [], 0
    for batch in raw_frame_batches(directory, capture):
        count += batch.shape[0]
        if count > 4096 or count * capture.line_bytes > 256 * 1024 * 1024:
            raise ValueError("Snapshot exceeds preview memory budget")
        chunks.append(batch)
    if not chunks:
        raise ValueError("No valid raw image frames")
    return capture, np.concatenate(chunks, axis=0) if len(chunks) > 1 else chunks[0]
