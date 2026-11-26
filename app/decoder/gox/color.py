"""GOX colour-calibration values and unclipped linear normalization.

This module neither reads nor writes files. ``frame_exposure_from_device``
interprets an already-loaded device.json document. Applying a calibration to
other frames (exposure-ratio scaling, batch correction) is done offline by
PhenomateDecoders; this app only fits, stores (``ColorCalibration.to_dict`` into
reference.json) and previews it.

``exposure_time_us`` means the actual integration time: the ExposureTime
readback plus the driver's documented offset (device.json
``derived.actual_integration_us``). ``gain`` is the Gain[AnalogAll]
magnification (x1.0..x126.0), not dB.
"""

from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any

import numpy as np

_FULL_SCALE_DN = 4095
_MATRIX_CONVENTION = "row_vector_camera_rgb @ matrix -> linear_srgb_d65"


def _positive(value: Any, name: str) -> float:
    if isinstance(value, bool):
        raise ValueError(f"{name} must be a finite positive number")
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{name} must be a finite positive number") from error
    if not math.isfinite(number) or number <= 0:
        raise ValueError(f"{name} must be a finite positive number")
    return number


@dataclass(frozen=True)
class CameraRGBNormalization:
    """``dn_over_full_scale``: DN / 4095 (pedestal unknown or none).
    ``pedestal_subtracted``: (DN - B) / (4095 - B) with a scalar ``measured`` or
    ``documented`` pedestal B (e.g. the GOX manual's 8LSB@8bit = 128 DN at 12 bit).
    """

    version: str = "dn_over_full_scale"
    pedestal_status: str = "unknown"
    pedestal_dn: float | None = None
    full_scale_dn: int = _FULL_SCALE_DN

    def __post_init__(self) -> None:
        if self.full_scale_dn != _FULL_SCALE_DN:
            raise ValueError("GOX normalization requires full_scale_dn=4095 for BayerRG12Packed")
        if self.version == "dn_over_full_scale":
            if self.pedestal_status not in ("unknown", "none") or self.pedestal_dn is not None:
                raise ValueError("dn_over_full_scale requires unknown/none pedestal_status and null pedestal_dn")
        elif self.version == "pedestal_subtracted":
            if self.pedestal_status not in ("measured", "documented"):
                raise ValueError("pedestal_subtracted requires a measured or documented pedestal")
            if isinstance(self.pedestal_dn, (bool, list, tuple)) or self.pedestal_dn is None:
                raise ValueError("pedestal_dn must be one scalar value")
            pedestal = float(self.pedestal_dn)
            if not math.isfinite(pedestal) or not 0 <= pedestal < self.full_scale_dn:
                raise ValueError("pedestal_dn must be a finite value below full_scale_dn")
            object.__setattr__(self, "pedestal_dn", pedestal)
        else:
            raise ValueError("Unsupported camera RGB normalization version")

    def to_dict(self) -> dict:
        return {"version": self.version, "pedestal_status": self.pedestal_status,
                "pedestal_dn": self.pedestal_dn, "full_scale_dn": self.full_scale_dn}

    @classmethod
    def from_dict(cls, value: Mapping) -> CameraRGBNormalization:
        return cls(value["version"], value["pedestal_status"], value["pedestal_dn"], value["full_scale_dn"])


@dataclass(frozen=True)
class ColorCalibration:
    """A fitted matrix with its metadata, as written into reference.json."""

    calibration_id: str
    matrix: np.ndarray
    normalization: CameraRGBNormalization
    applicability: Mapping[str, Any]
    fit: Mapping[str, Any]
    source: Mapping[str, Any]
    schema_version: int = 1
    matrix_convention: str = _MATRIX_CONVENTION

    def __post_init__(self) -> None:
        matrix = np.array(self.matrix, dtype=np.float64)
        if matrix.shape != (3, 3) or not np.isfinite(matrix).all():
            raise ValueError("matrix must be a finite 3x3 array")
        try:
            usable = np.linalg.matrix_rank(matrix) == 3 and np.linalg.cond(matrix) <= 1e6
        except np.linalg.LinAlgError as error:
            raise ValueError("matrix invertibility could not be established") from error
        if not usable:
            raise ValueError("matrix must be invertible with condition number at most 1e6")
        object.__setattr__(self, "matrix", matrix)

    def to_dict(self) -> dict:
        return {
            "schema_version": self.schema_version,
            "calibration_id": self.calibration_id,
            "matrix": self.matrix.tolist(),
            "matrix_convention": self.matrix_convention,
            "normalization": self.normalization.to_dict(),
            "applicability": dict(self.applicability),
            "fit": dict(self.fit),
            "source": dict(self.source),
        }


def _normalize(values: np.ndarray, normalization: CameraRGBNormalization, dtype: type) -> np.ndarray:
    result = values.astype(dtype, copy=True)
    if normalization.version == "pedestal_subtracted":
        result -= dtype(normalization.pedestal_dn)
        result /= dtype(normalization.full_scale_dn - normalization.pedestal_dn)
    else:
        result /= dtype(normalization.full_scale_dn)
    return result


def normalize_camera_rgb(rgb_dn: np.ndarray, normalization: CameraRGBNormalization) -> np.ndarray:
    """Native uint16 RGB DN to float32, without clipping (values below the pedestal stay negative)."""
    if (not isinstance(rgb_dn, np.ndarray) or rgb_dn.dtype != np.uint16 or rgb_dn.ndim < 1
            or rgb_dn.shape[-1] != 3 or rgb_dn.size == 0):
        raise ValueError("Camera RGB requires nonempty uint16 (..., 3) DN")
    return _normalize(rgb_dn, normalization, np.float32)


def normalize_patch_means(means_dn: np.ndarray, normalization: CameraRGBNormalization) -> np.ndarray:
    """float64-aggregated native patch means, normalized without reducing precision."""
    if (not isinstance(means_dn, np.ndarray) or not np.issubdtype(means_dn.dtype, np.floating)
            or means_dn.ndim != 2 or means_dn.shape[1] != 3 or means_dn.size == 0
            or not np.isfinite(means_dn).all()):
        raise ValueError("Patch means require a nonempty finite floating Nx3 DN array")
    return _normalize(means_dn, normalization, np.float64)


@dataclass(frozen=True)
class FrameExposure:
    """Exposure context of a capture: device identity, integration time and gain."""

    device_id: str
    device_model: str
    exposure_time_us: float  # actual integration time, see module docstring
    gain: float  # Gain[AnalogAll] magnification

    def __post_init__(self) -> None:
        for name in ("device_id", "device_model"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"Frame exposure requires a known {name}")
        object.__setattr__(self, "exposure_time_us", _positive(self.exposure_time_us, "frame exposure_time_us"))
        object.__setattr__(self, "gain", _positive(self.gain, "frame gain"))

    def to_dict(self) -> dict:
        return {"device_id": self.device_id, "device_model": self.device_model,
                "exposure_time_us": self.exposure_time_us, "gain": self.gain}


def frame_exposure_from_device(device: Mapping) -> FrameExposure:
    """Interpret a GOX device.json document; no defaults are substituted.

    Gain comes from the final ``applied[]`` readback of ``Gain``. Without a
    configured gain there is no readback, and the gain is not assumed x1.0.
    """
    if not isinstance(device, Mapping):
        raise ValueError("device metadata must be a JSON object")
    identity = device.get("identity") if isinstance(device.get("identity"), Mapping) else {}
    derived = device.get("derived") if isinstance(device.get("derived"), Mapping) else {}
    integration = derived.get("actual_integration_us")
    if integration is None:
        raise ValueError("device.json has no derived.actual_integration_us (ExposureTime readback)")
    gains = [entry for entry in device.get("applied") or ()
             if isinstance(entry, Mapping) and entry.get("name") == "Gain"]
    if not gains:
        raise ValueError("device.json has no Gain readback; set acquisition.gain explicitly to record it")
    return FrameExposure(
        device_id=identity.get("DeviceSerialNumber"),
        device_model=identity.get("DeviceModelName"),
        exposure_time_us=_positive(str(integration).strip(), "device.json actual_integration_us readback"),
        gain=_positive(str(gains[-1].get("readback")).strip(), "device.json Gain readback"),
    )
