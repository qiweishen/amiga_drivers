"""Explicit GOX color-calibration values and unclipped linear arithmetic.

This module neither reads nor writes files. ``frame_exposure_from_device``
interprets an already-loaded device.json document. ``correct_linear`` checks
the frame's device and exposure against ``applicability``: policy ``none``
applies only at the calibration exposure, and ``scale_by_exposure_ratio``
scales normalized linear camera RGB by (t_cal * g_cal) / (t_frame * g_frame).
That scaling is exact only when the black level is zero, so it requires a
measured or documented (manual-derived) pedestal, or the explicit ``pedestal_status="none"`` (zero) decision.

``exposure_time_us`` means the actual integration time: the ExposureTime
readback plus the driver's documented offset (device.json
``derived.actual_integration_us``). ``gain`` is the Gain[AnalogAll]
magnification (x1.0..x126.0), not dB.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import math
from numbers import Real
from types import MappingProxyType
from typing import Any

import numpy as np


_FULL_SCALE_DN = 4095
_MATRIX_CONVENTION = "row_vector_camera_rgb @ matrix -> linear_srgb_d65"
_LINEAR_AUDIT_FEATURES = (
    "LUTMode", "GradationCompressionMode", "ColorTransformationRGBMode", "BalanceWhiteAuto",
)
_PATCH_COUNTS = {"MCC24": 24, "SG140": 140, "VINYL18": 18}
_APPLICABILITY_KEYS = {
    "device_id", "device_model", "pixel_format", "exposure_time_us", "gain",
    "raw_audit", "exposure_compensation",
}
_FIT_KEYS = {"chart_key", "loss_de2000_rms", "used_patches"}
_SOURCE_KEYS = {"session_id", "segment", "record_offset", "sequence", "block_id"}


def _require_keys(value: Mapping, required: set[str], name: str, *, exact: bool = False) -> None:
    if not isinstance(value, Mapping):
        raise ValueError(f"{name} must be a JSON object")
    if not required.issubset(value):
        raise ValueError(f"{name} is missing required fields: {', '.join(sorted(required - value.keys()))}")
    if exact and set(value) != required:
        raise ValueError(f"{name} contains unsupported fields")


def _freeze_json(value: Any, name: str) -> Any:
    """Copy JSON metadata into recursively immutable containers."""
    if value is None or isinstance(value, (str, bool)):
        return value
    if type(value) is int:
        return value
    if type(value) is float:
        if not math.isfinite(value):
            raise ValueError(f"{name} must not contain NaN or infinity")
        return value
    if isinstance(value, Mapping):
        if any(not isinstance(key, str) for key in value):
            raise ValueError(f"{name} must have string object keys")
        return MappingProxyType({key: _freeze_json(item, f"{name}.{key}")
                                 for key, item in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(_freeze_json(item, name) for item in value)
    raise ValueError(f"{name} contains a value that is not JSON data")


def _thaw_json(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _thaw_json(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_thaw_json(item) for item in value]
    return value


def _reject_boolean_coefficients(value: Any) -> None:
    """Inspect sequences before NumPy can promote mixed bool/number entries."""
    if isinstance(value, (bool, np.bool_)) or (isinstance(value, np.ndarray) and value.dtype.kind == "b"):
        raise ValueError("matrix coefficients must not be booleans")
    if isinstance(value, (list, tuple)):
        for item in value:
            _reject_boolean_coefficients(item)


def _number(value: Any, name: str, *, nullable: bool = False, positive: bool = False) -> None:
    if value is None and nullable:
        return
    try:
        finite = math.isfinite(value)
    except (TypeError, ValueError, OverflowError):
        finite = False
    if (isinstance(value, (bool, np.bool_)) or not isinstance(value, Real)
            or not finite or (value <= 0 if positive else value < 0)):
        qualifier = "positive" if positive else "nonnegative"
        raise ValueError(f"{name} must be a finite {qualifier} number")


def _optional_text(value: Any, name: str) -> None:
    if value is not None and (not isinstance(value, str) or not value.strip()):
        raise ValueError(f"{name} must be a nonempty string or null when unknown")


def _optional_index(value: Any, name: str) -> None:
    if value is not None and (type(value) is not int or value < 0):
        raise ValueError(f"{name} must be a nonnegative integer or null when unknown")


@dataclass(frozen=True)
class CameraRGBNormalization:
    """Versioned normalization; unknown pedestal explicitly means no subtraction.

    ``dn_over_full_scale`` computes DN / full_scale. ``pedestal_subtracted``
    computes (DN - B) / (full_scale - B) with a scalar pedestal that is either
    ``measured`` or ``documented`` (a specification value, e.g. the GOX manual's
    8LSB@8bit rescaled to 128 DN at 12 bit).
    A measured [R, Gr, Gb, B] pedestal can be stored, but this RGB API refuses
    to apply it: CFA-domain processing must be implemented separately.
    """

    version: str = "dn_over_full_scale"
    pedestal_status: str = "unknown"
    pedestal_dn: float | tuple[float, float, float, float] | None = None
    full_scale_dn: int = _FULL_SCALE_DN

    def __post_init__(self) -> None:
        self.validate()
        pedestal = self.pedestal_dn
        if isinstance(pedestal, (list, tuple)):
            object.__setattr__(self, "pedestal_dn", tuple(float(value) for value in pedestal))
        elif pedestal is not None:
            object.__setattr__(self, "pedestal_dn", float(pedestal))

    def validate(self) -> None:
        if type(self.full_scale_dn) is not int or self.full_scale_dn != _FULL_SCALE_DN:
            raise ValueError("GOX normalization requires full_scale_dn=4095 for BayerRG12Packed")
        if self.version == "dn_over_full_scale":
            if self.pedestal_status not in ("unknown", "none") or self.pedestal_dn is not None:
                raise ValueError("dn_over_full_scale requires unknown/none pedestal_status and null pedestal_dn")
            return
        if self.version != "pedestal_subtracted":
            raise ValueError("Unsupported camera RGB normalization version")
        if self.pedestal_status not in ("measured", "documented") or self.pedestal_dn is None:
            raise ValueError("pedestal_subtracted requires a measured or documented pedestal")
        if self.pedestal_status == "documented" and isinstance(self.pedestal_dn, (list, tuple)):
            raise ValueError("A documented pedestal is one scalar specification value")
        values = self.pedestal_dn if isinstance(self.pedestal_dn, (list, tuple)) else (self.pedestal_dn,)
        if len(values) not in (1, 4) or (isinstance(self.pedestal_dn, (list, tuple)) and len(values) != 4):
            raise ValueError("pedestal_dn must be a scalar or [R, Gr, Gb, B], not an RGB triple")
        for value in values:
            _number(value, "pedestal_dn")
            if value >= self.full_scale_dn:
                raise ValueError("pedestal_dn must be below full_scale_dn")

    def to_dict(self) -> dict:
        return {
            "version": self.version,
            "pedestal_status": self.pedestal_status,
            "pedestal_dn": list(self.pedestal_dn) if isinstance(self.pedestal_dn, tuple) else self.pedestal_dn,
            "full_scale_dn": self.full_scale_dn,
        }

    @classmethod
    def from_dict(cls, value: Mapping) -> CameraRGBNormalization:
        _require_keys(value, {"version", "pedestal_status", "pedestal_dn", "full_scale_dn"},
                      "normalization", exact=True)
        return cls(**value)


@dataclass(frozen=True)
class ColorCalibration:
    """An immutable calibration object with pure JSON conversion interfaces.

    Unknown historical metadata remains null. Such objects can be inspected or
    serialized, but incomplete applicability cannot be used by correct_linear.
    """

    calibration_id: str
    matrix: np.ndarray
    normalization: CameraRGBNormalization
    applicability: Mapping[str, Any]
    fit: Mapping[str, Any]
    source: Mapping[str, Any]
    schema_version: int = 1
    matrix_convention: str = _MATRIX_CONVENTION

    def __post_init__(self) -> None:
        if type(self.schema_version) is not int or self.schema_version != 1:
            raise ValueError("Unsupported ColorCalibration schema_version")
        if not isinstance(self.calibration_id, str) or not self.calibration_id.strip():
            raise ValueError("calibration_id must be a nonempty string")
        if self.matrix_convention != _MATRIX_CONVENTION:
            raise ValueError("Unsupported color matrix convention")
        if not isinstance(self.normalization, CameraRGBNormalization):
            raise ValueError("normalization must be a CameraRGBNormalization")
        self.normalization.validate()
        try:
            _reject_boolean_coefficients(self.matrix)
            supplied = np.asarray(self.matrix)
            if supplied.dtype.kind not in "iuf":
                raise ValueError("matrix coefficients must be real numbers, not strings, booleans or complex values")
            with np.errstate(over="ignore", invalid="ignore"):
                matrix = np.array(supplied, dtype=np.float64, copy=True)
        except (TypeError, ValueError, OverflowError) as error:
            raise ValueError("matrix must contain numeric RGB coefficients") from error
        if matrix.shape != (3, 3) or not np.isfinite(matrix).all():
            raise ValueError("matrix must be a finite 3x3 array")
        try:
            usable = np.linalg.matrix_rank(matrix) == 3 and np.linalg.cond(matrix) <= 1e6
        except np.linalg.LinAlgError as error:
            raise ValueError("matrix invertibility could not be established") from error
        if not usable:
            raise ValueError("matrix must be invertible with condition number at most 1e6")
        # Immutable bytes backing also prevents callers from re-enabling writes.
        object.__setattr__(self, "matrix", np.frombuffer(matrix.tobytes(), dtype=np.float64).reshape(3, 3))
        for name, keys in (("applicability", _APPLICABILITY_KEYS), ("fit", _FIT_KEYS), ("source", _SOURCE_KEYS)):
            value = getattr(self, name)
            _require_keys(value, keys, name)
            object.__setattr__(self, name, _freeze_json(value, name))
        self._validate_metadata()

    def _validate_metadata(self) -> None:
        applicability = self.applicability
        for name in ("device_id", "device_model"):
            _optional_text(applicability[name], f"applicability.{name}")
        if applicability["pixel_format"] != "BayerRG12Packed":
            raise ValueError("Calibration applicability requires pixel_format=BayerRG12Packed")
        for name in ("exposure_time_us", "gain"):
            _number(applicability[name], f"applicability.{name}", nullable=True, positive=True)
        audit = applicability["raw_audit"]
        if not isinstance(audit, Mapping) or any(not isinstance(entry, Mapping) for entry in audit.values()):
            raise ValueError("applicability.raw_audit must contain feature audit objects")
        policy = applicability["exposure_compensation"]
        if policy not in ("none", "scale_by_exposure_ratio"):
            raise ValueError("Unsupported exposure_compensation policy")
        if (policy == "scale_by_exposure_ratio"
                and self.normalization.pedestal_status not in ("measured", "documented", "none")):
            raise ValueError("Exposure-ratio compensation requires a measured, documented or explicitly zero black level")
        chart = self.fit["chart_key"]
        if not isinstance(chart, str) or chart not in _PATCH_COUNTS:
            raise ValueError("fit.chart_key must be MCC24, SG140 or VINYL18")
        _number(self.fit["loss_de2000_rms"], "fit.loss_de2000_rms", nullable=True)
        used = self.fit["used_patches"]
        if type(used) is not int or not 1 <= used <= _PATCH_COUNTS[chart]:
            raise ValueError("fit.used_patches must be a positive integer within the chart patch count")
        for name in ("session_id", "segment"):
            _optional_text(self.source[name], f"source.{name}")
        for name in ("record_offset", "sequence", "block_id"):
            _optional_index(self.source[name], f"source.{name}")

    def validate(self) -> None:
        """Validate the already immutable object's metadata and normalization."""
        self.normalization.validate()
        self._validate_metadata()

    def to_dict(self) -> dict:
        return {
            "schema_version": self.schema_version,
            "calibration_id": self.calibration_id,
            "matrix": self.matrix.tolist(),
            "matrix_convention": self.matrix_convention,
            "normalization": self.normalization.to_dict(),
            "applicability": _thaw_json(self.applicability),
            "fit": _thaw_json(self.fit),
            "source": _thaw_json(self.source),
        }

    @classmethod
    def from_dict(cls, value: Mapping) -> ColorCalibration:
        _require_keys(value, {"schema_version", "calibration_id", "matrix", "matrix_convention",
                              "normalization", "applicability", "fit", "source"},
                      "calibration", exact=True)
        fields = dict(value)
        fields["normalization"] = CameraRGBNormalization.from_dict(fields["normalization"])
        return cls(**fields)


def _normalization(value: ColorCalibration | CameraRGBNormalization) -> CameraRGBNormalization:
    if isinstance(value, ColorCalibration):
        return value.normalization
    if isinstance(value, CameraRGBNormalization):
        return value
    raise ValueError("Expected a ColorCalibration or CameraRGBNormalization")


def _normalize(values: np.ndarray, normalization: CameraRGBNormalization, dtype: type) -> np.ndarray:
    pedestal = normalization.pedestal_dn
    if isinstance(pedestal, tuple):
        raise ValueError("[R, Gr, Gb, B] pedestal requires CFA-domain processing; RGB normalization cannot apply it")
    with np.errstate(over="ignore", invalid="ignore", divide="ignore"):
        result = values.astype(dtype, copy=True)
        if normalization.version == "pedestal_subtracted":
            working_pedestal = dtype(pedestal)
            divisor = dtype(normalization.full_scale_dn - pedestal)
            if dtype is np.float32 and working_pedestal >= dtype(normalization.full_scale_dn):
                raise ValueError("float32 cannot distinguish the measured pedestal from full scale")
            if not np.isfinite(divisor) or divisor <= 0:
                raise ValueError("Normalization requires finite positive pedestal headroom")
            result -= working_pedestal
            result /= divisor
        else:
            result /= dtype(normalization.full_scale_dn)
    if not np.isfinite(result).all():
        raise ValueError("Normalization produced nonfinite RGB values")
    return result


def normalize_camera_rgb(
    rgb_dn: np.ndarray, cal_or_normalization: ColorCalibration | CameraRGBNormalization,
) -> np.ndarray:
    """Normalize native uint16 RGB DN to float32 without clipping or exposure scaling."""
    normalization = _normalization(cal_or_normalization)
    if (not isinstance(rgb_dn, np.ndarray) or rgb_dn.dtype != np.uint16 or rgb_dn.ndim < 1
            or rgb_dn.shape[-1] != 3 or rgb_dn.size == 0
            or int(rgb_dn.max()) > normalization.full_scale_dn):
        raise ValueError("Camera RGB requires nonempty uint16 (..., 3) DN within 0..4095")
    return _normalize(rgb_dn, normalization, np.float32)


def normalize_patch_means(
    means_dn: np.ndarray, cal_or_normalization: ColorCalibration | CameraRGBNormalization,
) -> np.ndarray:
    """Normalize float64-aggregated native patch means without reducing precision."""
    normalization = _normalization(cal_or_normalization)
    if (not isinstance(means_dn, np.ndarray) or not np.issubdtype(means_dn.dtype, np.floating)
            or means_dn.ndim != 2 or means_dn.shape[1] != 3 or means_dn.size == 0
            or not np.isfinite(means_dn).all() or np.any(means_dn < 0)
            or np.any(means_dn > normalization.full_scale_dn)):
        raise ValueError("Patch means require a nonempty finite floating Nx3 DN array within 0..4095")
    return _normalize(means_dn, normalization, np.float64)


_EXPOSURE_MATCH_REL = 1e-3


@dataclass(frozen=True)
class FrameExposure:
    """Exposure context of the frames a calibration is applied to."""

    device_id: str
    device_model: str
    exposure_time_us: float  # actual integration time, see module docstring
    gain: float  # Gain[AnalogAll] magnification

    def __post_init__(self) -> None:
        _optional_text(self.device_id, "frame device_id")
        _optional_text(self.device_model, "frame device_model")
        if self.device_id is None or self.device_model is None:
            raise ValueError("Frame exposure requires a known device_id and device_model")
        _number(self.exposure_time_us, "frame exposure_time_us", positive=True)
        _number(self.gain, "frame gain", positive=True)
        object.__setattr__(self, "exposure_time_us", float(self.exposure_time_us))
        object.__setattr__(self, "gain", float(self.gain))

    def to_dict(self) -> dict:
        return {"device_id": self.device_id, "device_model": self.device_model,
                "exposure_time_us": self.exposure_time_us, "gain": self.gain}


def _readback_number(text: Any, name: str) -> float:
    try:
        value = float(str(text).strip())
    except ValueError as error:
        raise ValueError(f"device.json {name} readback is not a number: {text!r}") from error
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f"device.json {name} readback must be a finite positive number")
    return value


def frame_exposure_from_device(device: Mapping) -> FrameExposure:
    """Interpret a GOX device.json document; no defaults are substituted.

    Gain comes from the final ``applied[]`` readback of ``Gain`` (written with
    the factory Analog All selector). Without a configured gain there is no
    readback and the gain is reported as unknown rather than assumed x1.0.
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
        exposure_time_us=_readback_number(integration, "actual_integration_us"),
        gain=_readback_number(gains[-1].get("readback"), "Gain"),
    )


def exposure_scale(cal: ColorCalibration, frame: FrameExposure) -> float:
    """Multiplier that maps frame-normalized RGB to the calibration exposure."""
    applicability = cal.applicability
    for name in ("exposure_time_us", "gain"):
        if applicability[name] is None:
            raise ValueError(f"Cannot compare exposure with unknown applicability.{name}")
    return ((applicability["exposure_time_us"] * applicability["gain"])
            / (frame.exposure_time_us * frame.gain))


def correct_linear(rgb_dn: np.ndarray, cal: ColorCalibration,
                   frame: FrameExposure | None = None) -> np.ndarray:
    """Return unclipped float32 linear sRGB, referred to the calibration exposure.

    Policy ``none`` applies only when the frame's exposure and gain match the
    calibration (within 0.1 %); without ``frame`` the caller must have verified
    that. Policy ``scale_by_exposure_ratio`` requires ``frame`` and scales the
    normalized camera RGB by ``exposure_scale`` before the matrix.
    """
    if not isinstance(cal, ColorCalibration):
        raise ValueError("correct_linear requires a ColorCalibration")
    if frame is not None and not isinstance(frame, FrameExposure):
        raise ValueError("frame must be a FrameExposure")
    applicability = cal.applicability
    policy = applicability["exposure_compensation"]
    if policy == "scale_by_exposure_ratio" and frame is None:
        raise ValueError("Exposure-ratio compensation requires the frame's FrameExposure")
    for name in ("device_id", "device_model", "exposure_time_us", "gain"):
        if applicability[name] is None:
            raise ValueError(f"Cannot apply calibration with unknown applicability.{name}")
    for name in _LINEAR_AUDIT_FEATURES:
        entry = applicability["raw_audit"].get(name)
        if (not isinstance(entry, Mapping) or entry.get("outcome") != "match"
                or str(entry.get("value", "")).strip().lower() not in ("off", "false", "0")):
            raise ValueError(f"Cannot apply calibration without a successful {name}=Off audit")
    scale = 1.0
    if frame is not None:
        for name in ("device_id", "device_model"):
            if getattr(frame, name) != applicability[name]:
                raise ValueError(f"Frame {name} {getattr(frame, name)!r} does not match the calibration "
                                 f"({applicability[name]!r})")
        scale = exposure_scale(cal, frame)
        if policy == "none" and abs(scale - 1.0) > _EXPOSURE_MATCH_REL:
            raise ValueError("Frame exposure x gain differs from the calibration and its policy is 'none'")
        if policy == "none":
            scale = 1.0
    normalized = normalize_camera_rgb(rgb_dn, cal)
    if scale != 1.0:
        normalized *= np.float32(scale)
    with np.errstate(over="ignore", invalid="ignore"):
        corrected = normalized @ cal.matrix.astype(np.float32)
    if not np.isfinite(corrected).all():
        raise ValueError("Color correction produced nonfinite float32 RGB values")
    return corrected
