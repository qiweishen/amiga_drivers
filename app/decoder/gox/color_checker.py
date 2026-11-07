"""Fit a GOX reference chart without changing the recorded native samples.

OpenCV detects geometry on a temporary BGR8 image. That image is used only
for locating patches; measurements use full-resolution BayerRG12Packed-derived uint16 RGB.
The fitted matrix maps normalized linear camera RGB to linear sRGB (D65).
Because the fit is absolute, the matrix carries a luminance gain that depends
on the chart capture's exposure; ``render`` only clips the linear result to
[0, 1] and applies the sRGB transfer (no exposure adjustment). The GUI receives
small 8-bit screen previews;
``full_resolution_png`` regenerates lossless 16-bit PNGs from the saved RAW.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import cv2 as cv
import numpy as np

from ..ebus import recording as raw_recording
from . import delta_e, recording, render
from .color import (
    CameraRGBNormalization,
    ColorCalibration,
    FrameExposure,
    frame_exposure_from_device,
    normalize_camera_rgb,
    normalize_patch_means,
)
from .preview import (
    DISPLAY_LONG_EDGE,
    display_points,
    native_display_png,
    native_display_rgb8,
    native_full_png16,
    native_rgb,
    png8_bytes,
)


@dataclass(frozen=True)
class ChartSpec:
    key: str
    label: str
    patch_count: int
    image_filename: str


CHARTS: dict[str, ChartSpec] = {
    "MCC24": ChartSpec("MCC24", "Macbeth / ColorChecker Classic · 24 patches", 24,
                       "Standard_Macbeth_Chart.png"),
    "SG140": ChartSpec("SG140", "ColorChecker Digital SG · 140 patches", 140,
                       "Digital_SG_140.png"),
    "VINYL18": ChartSpec("VINYL18", "DKK / Vinyl · 18 patches", 18,
                         "DKK_Color_Chart.png"),
}
# X-Rite ColorChecker Classic patch names, row-major from dark skin (1) to black (24).
MCC24_PATCH_NAMES = (
    "Dark skin", "Light skin", "Blue sky", "Foliage", "Blue flower", "Bluish green",
    "Orange", "Purplish blue", "Moderate red", "Purple", "Yellow green", "Orange yellow",
    "Blue", "Green", "Red", "Yellow", "Magenta", "Cyan",
    "White 9.5", "Neutral 8", "Neutral 6.5", "Neutral 5", "Neutral 3.5", "Black 2",
)
_REFERENCE_CONSTANTS = {
    "MCC24": "COLORCHECKER_MACBETH",
    "SG140": "COLORCHECKER_DIGITAL_SG",
    "VINYL18": "COLORCHECKER_VINYL",
}
_DETECTION_EDGE = 1600
_MIN_PATCH_PIXELS = 16
_SATURATION_LIMIT = 0.98

# Black level: the GOX manual (Ver. 1.3, p.172, "Black Level Adjustment") gives the
# default level as 8LSB@8bit; the gox driver rescales it to 12 bit (8 x 16 = 128 DN)
# and records it in device.json derived.black_level. No register reports it.
DOCUMENTED_PEDESTAL_DN = 128.0
PEDESTAL_SOURCE = ("GOX manual Ver. 1.3 p.172 'Black Level Adjustment': Default Level 8LSB@8bit; "
                   "rescaled to 12 bit by the gox driver (8 x 16 = 128 DN, device.json "
                   "derived.black_level.pedestal_lsb12_derived). Not measured on this camera.")


@dataclass(frozen=True)
class CalibrationPreview:
    chart_key: str
    chart_label: str
    patch_count: int
    used_patches: int
    matrix: list[list[float]]
    loss: float
    # 8-bit on-screen PNGs (long edge <= preview.DISPLAY_LONG_EDGE). Full-resolution
    # 16-bit PNGs come from full_resolution_png(), re-derived from the saved RAW.
    original_png: bytes
    annotated_png: bytes
    corrected_png: bytes
    pixel_format: str
    full_scale: int
    provenance: dict
    # ColorCalibration.to_dict() for reuse on other frames, or None when the
    # reference exposure/gain could not be established (reason in provenance).
    color_calibration: dict | None = None
    # In-memory only: the source camera directory, for full-resolution downloads.
    source_dir: Path | None = None


def capability_reason() -> str | None:
    """Report missing APIs without loading a detector, model, or device."""
    if str(getattr(cv, "__version__", "")).split(".")[0] != "5":
        return "ColorChecker calibration requires the project's OpenCV 5 API."
    mcc, ccm = getattr(cv, "mcc", None), getattr(cv, "ccm", None)
    required = (
        (mcc, ("MCC24", "SG140", "VINYL18", "CCheckerDetector", "CChecker")),
        (ccm, ("ColorCorrectionModel", "COLORCHECKER_MACBETH", "COLORCHECKER_DIGITAL_SG",
               "COLORCHECKER_VINYL", "COLOR_SPACE_SRGB", "CCM_LINEAR",
               "LINEARIZATION_IDENTITY", "DISTANCE_CIE2000")),
        (getattr(mcc, "CCheckerDetector", None),
         ("create", "setColorChartType", "process", "getListColorChecker")),
        (getattr(mcc, "CChecker", None), ("getColorCharts", "getBox", "getTarget")),
        (getattr(ccm, "ColorCorrectionModel", None),
         ("setColorSpace", "setCcmType", "setLinearization", "setDistance",
          "setSaturatedThreshold", "compute", "getColorCorrectionMatrix", "getLoss", "getMask")),
    )
    for owner, names in required:
        if owner is None or any(not hasattr(owner, name) for name in names):
            return "This OpenCV build lacks the OpenCV 5 mcc/ccm APIs required for ColorChecker calibration."
    return None


def _linear_audit(directory: Path) -> dict:
    """Identity linearization needs recorded evidence that tone curves are off."""
    device = raw_recording.device_metadata(directory)
    audit = device.get("raw_audit", {})
    evidence = {}
    for name in ("LUTMode", "GradationCompressionMode", "ColorTransformationRGBMode", "BalanceWhiteAuto"):
        entry = audit.get(name) if isinstance(audit, dict) else None
        if not isinstance(entry, dict) or entry.get("outcome") != "match":
            raise ValueError(f"Color calibration requires a successful {name}=Off device audit.")
        value = str(entry.get("value", "")).strip().lower()
        if value not in {"off", "false", "0"}:
            raise ValueError(f"{name} is enabled; a stable linear DN color calibration cannot be fitted.")
        evidence[name] = dict(entry)
    return evidence


def _native_rgb(frame: recording.ImageFrame) -> tuple[np.ndarray, int, int]:
    return native_rgb(frame), 4095, 12


def _display_rgb(rgb: np.ndarray, full_scale: int) -> np.ndarray:
    """A reduced detection input only; previews and sampling use the native image."""
    height, width = rgb.shape[:2]
    scale = min(1.0, _DETECTION_EDGE / max(height, width))
    if scale < 1.0:
        rgb = cv.resize(rgb, (max(1, round(width * scale)), max(1, round(height * scale))),
                        interpolation=cv.INTER_AREA)
    return normalize_camera_rgb(rgb, CameraRGBNormalization(full_scale_dn=full_scale))


def _srgb_encode(linear: np.ndarray) -> np.ndarray:
    return render.srgb_oetf(linear)


def _bgr8(rgb: np.ndarray) -> np.ndarray:
    """The sole 8-bit image conversion: temporary input for cv.mcc geometry detection."""
    value = np.rint(np.clip(rgb, 0.0, 1.0) * 255.0).astype(np.uint8)
    return cv.cvtColor(value, cv.COLOR_RGB2BGR)


def _detect(display_rgb: np.ndarray, chart: ChartSpec) -> tuple[np.ndarray, str]:
    detector = cv.mcc.CCheckerDetector.create()
    detector.setColorChartType(getattr(cv.mcc, chart.key))
    transfer = "sRGB-like OETF on resized camera RGB; temporary BGR8 for geometry only"
    detected = detector.process(_bgr8(_srgb_encode(display_rgb)), 2)
    if not detected:
        # Detection-only gray-world retry; neither image supplies fitted colors.
        channel_means = display_rgb.mean(axis=(0, 1), dtype=np.float64)
        if np.all(channel_means > 0):
            balanced = display_rgb * (channel_means.mean() / channel_means)
            detected = detector.process(_bgr8(_srgb_encode(balanced)), 2)
            transfer = "gray-world gains then sRGB-like OETF; temporary BGR8 for geometry only"
    if not detected:
        raise ValueError(f"No {chart.key} chart detected. Show the full chart clearly and recapture.")
    checkers = detector.getListColorChecker()
    if not checkers:
        raise ValueError(f"No complete {chart.key} chart was returned by the detector.")
    if len(checkers) > 1:
        raise ValueError("More than one selected ColorChecker is visible. Keep one chart in view.")
    checker = checkers[0]
    if checker.getTarget() != getattr(cv.mcc, chart.key):
        raise ValueError("The detected chart does not match the selected ColorChecker format.")
    points = np.asarray(checker.getColorCharts(), dtype=np.float64)
    if points.size != chart.patch_count * 4 * 2 or not np.isfinite(points).all():
        raise ValueError("The detected ColorChecker has an invalid patch count or geometry.")
    return points.reshape(chart.patch_count, 4, 2), transfer


PATCH_CLIPPED = "clipped"  # a native photosite inside the patch is at full scale
PATCH_NEAR_SATURATION = "near_saturation"  # patch mean channel >= _SATURATION_LIMIT


def _require_unsaturated(status: list[str | None]) -> None:
    for index, state in enumerate(status, 1):
        if state == PATCH_CLIPPED:
            raise ValueError(f"ColorChecker patch {index} is saturated. Reduce exposure and recapture.")
        if state == PATCH_NEAR_SATURATION:
            raise ValueError(f"ColorChecker patch {index} is too close to saturation. Reduce exposure and recapture.")


def _sample(rgb: np.ndarray, raw: np.ndarray, polygons: np.ndarray,
            full_scale: int) -> tuple[np.ndarray, list[int]]:
    """Patch means for fitting; rejects any saturated patch."""
    means, counts, status = _sample_patches(rgb, raw, polygons, full_scale)
    _require_unsaturated(status)
    return means, counts


def _sample_patches(rgb: np.ndarray, raw: np.ndarray, polygons: np.ndarray,
                    full_scale: int) -> tuple[np.ndarray, list[int], list[str | None]]:
    """Patch means, pixel counts and saturation status; saturation is reported, not raised.

    A clipped patch's mean is a lower bound of its true signal.
    """
    height, width = rgb.shape[:2]
    normalization = CameraRGBNormalization(full_scale_dn=full_scale)
    means, counts, status = [], [], []
    for index, polygon in enumerate(polygons, 1):
        if (np.any(polygon < 0) or np.any(polygon[:, 0] > width - 1)
                or np.any(polygon[:, 1] > height - 1)):
            raise ValueError(f"ColorChecker patch {index} extends outside the frame.")
        contour = np.rint(polygon).astype(np.int32)
        if not cv.isContourConvex(contour) or abs(cv.contourArea(contour)) < _MIN_PATCH_PIXELS:
            raise ValueError(f"ColorChecker patch {index} is too small or has invalid geometry.")
        x0, y0 = contour.min(axis=0)
        x1, y1 = contour.max(axis=0) + 1
        # Boolean membership flags are independent of image intensity/bit depth.
        yy, xx = np.mgrid[y0:y1, x0:x1]
        selected = np.ones(xx.shape, dtype=bool)
        positive = cv.contourArea(contour, oriented=True) > 0
        for start, end in zip(contour, np.roll(contour, -1, axis=0)):
            cross = (end[0] - start[0]) * (yy - start[1]) - (end[1] - start[1]) * (xx - start[0])
            selected &= cross >= 0 if positive else cross <= 0
        count = int(np.count_nonzero(selected))
        if count < _MIN_PATCH_PIXELS:
            raise ValueError(f"ColorChecker patch {index} has too few native pixels to sample.")
        # Test the original photosites/components; demosaicing can conceal a clip.
        native_samples = raw[y0:y1, x0:x1][selected]
        mean_dn = rgb[y0:y1, x0:x1][selected].mean(axis=0, dtype=np.float64)
        # Keep float64 aggregation for fitting; share the image path's formula,
        # without quantizing measured DN means through a float32 image first.
        mean = normalize_patch_means(mean_dn.reshape(1, 3), normalization)[0]
        if np.any(native_samples >= full_scale):
            status.append(PATCH_CLIPPED)
        elif float(mean.max()) >= _SATURATION_LIMIT:
            status.append(PATCH_NEAR_SATURATION)
        else:
            status.append(None)
        means.append(mean)
        counts.append(count)
    return np.asarray(means, dtype=np.float64), counts, status


def _fit(means: np.ndarray, chart: ChartSpec) -> tuple[np.ndarray, float, int, np.ndarray]:
    """(matrix, RMS ΔE00 loss, patches used, reference linear sRGB (patch_count, 3))."""
    if (not np.isfinite(means).all() or np.linalg.matrix_rank(means) < 3
            or np.linalg.cond(means) > 1e5):
        raise ValueError("The chart measurements lack independent RGB information; check exposure and recapture.")
    reference = getattr(cv.ccm, _REFERENCE_CONSTANTS[chart.key])
    model = cv.ccm.ColorCorrectionModel(means.reshape(chart.patch_count, 1, 3), reference)
    model.setColorSpace(cv.ccm.COLOR_SPACE_SRGB)
    model.setCcmType(cv.ccm.CCM_LINEAR)
    model.setLinearization(cv.ccm.LINEARIZATION_IDENTITY)
    model.setDistance(cv.ccm.DISTANCE_CIE2000)
    model.setSaturatedThreshold(0.0, _SATURATION_LIMIT)
    model.compute()
    matrix = np.asarray(model.getColorCorrectionMatrix(), dtype=np.float64)
    loss = float(model.getLoss())
    mask = np.asarray(model.getMask()).reshape(-1)
    if mask.size != chart.patch_count or not np.all(mask):
        raise ValueError("OpenCV excluded chart measurements; recapture a fully exposed, unsaturated chart.")
    if (matrix.shape != (3, 3) or not np.isfinite(matrix).all() or not np.isfinite(loss)
            or loss < 0 or np.linalg.matrix_rank(matrix) < 3 or np.linalg.cond(matrix) > 1e6):
        raise ValueError("ColorChecker fitting did not produce a finite, usable color matrix.")
    reference_linear = np.asarray(model.getRefLinearRGB(), dtype=np.float64).reshape(-1, 3)
    if reference_linear.shape != (chart.patch_count, 3) or not np.isfinite(reference_linear).all():
        raise ValueError("OpenCV did not provide the chart's reference colors.")
    return matrix, loss, int(np.count_nonzero(mask)), reference_linear


def _patch_errors(fit_means: np.ndarray, matrix: np.ndarray, reference_linear: np.ndarray,
                  chart: ChartSpec) -> list[dict]:
    """Per-patch CIEDE2000 between the reference and the corrected chart measurement."""
    reference_lab = delta_e.lab_from_linear_srgb(reference_linear)
    corrected_lab = delta_e.lab_from_linear_srgb(fit_means @ matrix)
    errors = delta_e.ciede2000(reference_lab, corrected_lab)
    names = MCC24_PATCH_NAMES if chart.key == "MCC24" else ()
    return [{"patch": index + 1, "name": names[index] if names else None,
             "delta_e2000": float(errors[index]),
             "reference_lab": reference_lab[index].tolist(), "corrected_lab": corrected_lab[index].tolist()}
            for index in range(chart.patch_count)]


def _exposure_diagnostics(raw_means: np.ndarray, fit_means: np.ndarray, matrix: np.ndarray,
                          full_scale: int) -> dict:
    """How strongly the absolute fit brightens or darkens the chart's brightest patch.

    ``raw_means`` are DN / full scale (pedestal included); ``fit_means`` are the
    pedestal-subtracted matrix inputs. With gain g > 1, matrix inputs above 1/g
    exceed linear sRGB luminance 1.0 and clip in the preview. With g <= 1 the matrix
    does not push unclipped neutral input past display white; over-range output
    then comes from single saturated channels or clipped photosites.
    """
    index = int(np.argmax(raw_means.sum(axis=1)))
    peak = float(raw_means[index].max())
    fitted_peak = float(fit_means[index].max())
    output = fit_means[index] @ matrix
    luminance = float(output @ np.array([0.2126, 0.7152, 0.0722]))
    gain = luminance / fitted_peak if fitted_peak > 0 else None
    return {
        "brightest_patch": index + 1,
        "brightest_patch_peak_fraction_of_full_scale": peak,
        "brightest_patch_peak_dn": peak * full_scale,
        "brightest_patch_peak_matrix_input": fitted_peak,
        "brightest_patch_linear_srgb": output.tolist(),
        "brightest_patch_linear_srgb_luminance": luminance,
        "chart_exposure_gain": gain,
        "definition": "luminance(patch @ matrix) / max(patch pedestal-subtracted normalized camera RGB)",
    }


_OUTLINE_THICKNESS = 2


def _outline(image: np.ndarray, polygons: np.ndarray, value: tuple) -> np.ndarray:
    cv.polylines(image, [np.rint(poly).astype(np.int32) for poly in polygons],
                 True, value, _OUTLINE_THICKNESS, cv.LINE_8)
    return image


def _annotated_display_png(rgb: np.ndarray, polygons: np.ndarray) -> bytes:
    """Outlines drawn at display resolution so they stay visible after downscaling."""
    small = native_display_rgb8(rgb)
    return png8_bytes(_outline(small, display_points(polygons, rgb.shape, small.shape), (0, 255, 0)))


def _reference_black_level(directory: Path) -> dict:
    """The documented 128 DN pedestal, cross-checked against the capture's device.json."""
    record = {"pedestal_dn": DOCUMENTED_PEDESTAL_DN, "status": "documented", "source": PEDESTAL_SOURCE,
              "device_json": None, "warnings": []}
    try:
        documented = (raw_recording.device_metadata(directory).get("derived") or {}).get("black_level")
    except (OSError, ValueError) as error:
        documented = None
        record["warnings"].append(f"device.json black level unavailable ({error or type(error).__name__}); "
                                  f"using the documented {DOCUMENTED_PEDESTAL_DN:g} DN")
    if isinstance(documented, dict):
        record["device_json"] = documented
        derived = documented.get("pedestal_lsb12_derived")
        if derived != DOCUMENTED_PEDESTAL_DN:
            record["warnings"].append(f"device.json pedestal_lsb12_derived is {derived!r}, not "
                                      f"{DOCUMENTED_PEDESTAL_DN:g} DN; the documented value is used")
        register = documented.get("black_level_register")
        if register not in ("0", 0):
            record["warnings"].append(f"BlackLevel register reads {register!r}, not 0: the camera's offset "
                                      "differs from the default and 128 DN may be wrong; capture a dark frame")
    elif documented is None and not record["warnings"]:
        record["warnings"].append("device.json has no derived.black_level; using the documented "
                                  f"{DOCUMENTED_PEDESTAL_DN:g} DN")
    return record


def _reference_exposure(directory: Path) -> tuple[FrameExposure | None, str]:
    """The reference capture's exposure context, or None with the reason."""
    try:
        return frame_exposure_from_device(raw_recording.device_metadata(directory)), ""
    except (OSError, ValueError) as error:
        return None, str(error) or type(error).__name__


def _color_calibration(directory: Path, chart: ChartSpec, frame: recording.ImageFrame,
                       matrix: np.ndarray, normalization: CameraRGBNormalization, loss: float,
                       used: int, evidence: dict, exposure: FrameExposure) -> ColorCalibration:
    record = frame.record
    return ColorCalibration(
        calibration_id=f"gox-{exposure.device_id}-{chart.key}-{record.hrt}",
        matrix=matrix, normalization=normalization,
        applicability={
            **exposure.to_dict(), "pixel_format": frame.pixel_format, "raw_audit": evidence,
            "exposure_compensation": "scale_by_exposure_ratio",
        },
        fit={"chart_key": chart.key, "loss_de2000_rms": loss, "used_patches": used},
        source={"session_id": directory.as_posix(), "segment": record.path.name,
                "record_offset": record.offset, "sequence": record.sequence, "block_id": record.bid},
    )


GEOMETRY_DETECTED = "detected"
GEOMETRY_SUPPLIED = "supplied"


@dataclass(frozen=True)
class ChartMeasurement:
    """Native patch measurements of one reference capture; nothing is fitted yet.

    Holds the full-resolution native RGB, so keep it only while it is needed.
    """

    directory: Path
    chart: ChartSpec
    frame: recording.ImageFrame = field(repr=False)
    rgb: np.ndarray = field(repr=False)
    full_scale: int
    bits: int
    evidence: dict
    polygons: np.ndarray = field(repr=False)
    geometry: str  # GEOMETRY_DETECTED, or GEOMETRY_SUPPLIED from another capture of the same scene
    detection_transfer: str
    means: np.ndarray  # normalized camera RGB, float64 (patch_count, 3)
    counts: list[int]
    status: list[str | None]  # per patch: None, PATCH_CLIPPED or PATCH_NEAR_SATURATION


def measure(directory: Path, chart_key: str, *, polygons: np.ndarray | None = None) -> ChartMeasurement:
    """Detect (or reuse) chart geometry and sample native patch means; saturation is reported.

    ``polygons`` (native pixels) skips detection, e.g. for an exposure bracket of a
    static scene where one exposure is too dark or bright for the detector.
    """
    if not isinstance(chart_key, str) or chart_key not in CHARTS:
        raise ValueError("Select MCC24, SG140, or VINYL18 before capturing the ColorChecker.")
    reason = capability_reason()
    if reason:
        raise ValueError(reason)
    directory = Path(directory)
    evidence = _linear_audit(directory)
    frame = recording.snapshot_frame(directory, verify_crc=True)
    if frame.skipped_invalid:
        raise ValueError("The reference contains an invalid received buffer; recapture the ColorChecker.")
    rgb, full_scale, bits = _native_rgb(frame)
    chart = CHARTS[chart_key]
    if polygons is None:
        display = _display_rgb(rgb, full_scale)
        preview_polygons, detection_transfer = _detect(display, chart)
        ratios = np.array([rgb.shape[1] / display.shape[1], rgb.shape[0] / display.shape[0]])
        # cv.resize maps pixel centers, including the half-pixel origin offset.
        polygons = (preview_polygons + 0.5) * ratios - 0.5
        geometry = GEOMETRY_DETECTED
    else:
        polygons = np.asarray(polygons, dtype=np.float64)
        if polygons.shape != (chart.patch_count, 4, 2) or not np.isfinite(polygons).all():
            raise ValueError("Supplied chart geometry does not match the selected chart")
        detection_transfer = "not run; geometry supplied from another capture of the same scene"
        geometry = GEOMETRY_SUPPLIED
    means, counts, status = _sample_patches(rgb, frame.image, polygons, full_scale)
    return ChartMeasurement(directory, chart, frame, rgb, full_scale, bits, evidence, polygons,
                            geometry, detection_transfer, means, counts, status)


def calibrate(directory: Path, chart_key: str, *, measurement: ChartMeasurement | None = None) -> CalibrationPreview:
    """Read one fresh reference capture and return only in-memory GUI products."""
    if measurement is None:
        measurement = measure(directory, chart_key)
    elif measurement.chart.key != chart_key or measurement.directory != Path(directory):
        raise ValueError("The supplied measurement belongs to another capture or chart")
    directory = measurement.directory
    chart, frame, rgb = measurement.chart, measurement.frame, measurement.rgb
    full_scale, bits, evidence = measurement.full_scale, measurement.bits, measurement.evidence
    polygons, detection_transfer = measurement.polygons, measurement.detection_transfer
    _require_unsaturated(measurement.status)
    means, sample_counts = measurement.means, measurement.counts
    # Operator decision (2026-11-06): subtract the documented 128 DN pedestal by default.
    black_level = _reference_black_level(directory)
    pedestal = black_level["pedestal_dn"]
    normalization = CameraRGBNormalization(version="pedestal_subtracted", pedestal_status="documented",
                                           pedestal_dn=pedestal, full_scale_dn=full_scale)
    fit_means = (means * full_scale - pedestal) / (full_scale - pedestal)
    matrix, loss, used, reference_linear = _fit(fit_means, chart)
    patch_errors = _patch_errors(fit_means, matrix, reference_linear, chart)
    scene_linear = normalize_camera_rgb(rgb, normalization) @ matrix.astype(np.float32)
    corrected_png = render.display_png(scene_linear)
    del scene_linear
    original_png = native_display_png(rgb)
    annotated_png = _annotated_display_png(rgb, polygons)
    exposure, exposure_reason = _reference_exposure(directory)
    calibration = None
    if exposure is not None:
        try:
            calibration = _color_calibration(directory, chart, frame, matrix, normalization,
                                             loss, used, evidence, exposure).to_dict()
        except ValueError as error:
            exposure_reason = f"calibration record rejected: {error}"
    provenance = {
        "method": "opencv-5-color-correction-model",
        "opencv_version": str(cv.__version__),
        "reference_colors": _REFERENCE_CONSTANTS[chart_key],
        "target_color_space": "sRGB D65",
        "input_linearization": "identity",
        "normalization": normalization.to_dict(),
        "matrix_convention": "normalized camera RGB row vector @ matrix = linear sRGB",
        "display": dict(render.PROVENANCE),
        "exposure_diagnostics": _exposure_diagnostics(means, fit_means, matrix, full_scale),
        "black_level_policy": (f"documented pedestal {pedestal:g} DN subtracted before the fit and the "
                               "correction: (DN - pedestal) / (full scale - pedestal), no clipping"),
        "black_level": black_level,
        "reference_exposure": exposure.to_dict() if exposure else None,
        "reference_exposure_unavailable_reason": exposure_reason or None,
        "preview_encoding": (f"screen: 8-bit PNG area-downscaled to <= {DISPLAY_LONG_EDGE} px "
                             "(original and annotated: linear DN / 4095); download: full-resolution 16-bit "
                             "PNG regenerated from the saved RAW (original DN shifted left by 4, corrected "
                             "sRGB scaled to 65535)"),
        "display_precision": "browser compositor and display output precision are not verified",
        "fit_distance": "CIEDE2000",
        "fit_loss_definition": "root mean square CIEDE2000 over fitted reference patches",
        "patch_errors": patch_errors,
        "patch_errors_definition": ("CIEDE2000 (kL=kC=kH=1) between the OpenCV reference linear sRGB and "
                                    "(pedestal-subtracted patch mean @ matrix), both converted to "
                                    "CIE L*a*b* D65/2 deg; the RMS over fitted patches equals the fit loss"),
        "source_effective_bits": bits,
        "source_pixel_format": frame.pixel_format,
        "source_full_scale_dn": full_scale,
        "source_segment": frame.record.path.name,
        "source_record_offset": frame.record.offset,
        "source_sequence": frame.record.sequence,
        "source_block_id": frame.record.bid,
        "source_host_realtime_ns": frame.record.hrt,
        "source_payload_crc32c_verified": True,
        "linear_input_audit": evidence,
        "sampling": "native-depth RGB means inside the selected chart's central patch polygons",
        "demosaic": "BayerRG12Packed unpacked to 12-bit DN in uint16, OpenCV bilinear to uint16 BGR, then RGB",
        "detection_transfer": detection_transfer,
        "chart_geometry": measurement.geometry,
        "detection_image_used_for_calibration": False,
        "patch_rgb_normalized": means.tolist(),
        "patch_rgb_matrix_input": fit_means.tolist(),
        "patch_rgb_dn": (means * full_scale).tolist(),
        "patch_pixel_counts": sample_counts,
        "patch_polygons_native_pixels": polygons.tolist(),
        "saturation_upper_limit": _SATURATION_LIMIT,
        "raw_payload_modified": False,
    }
    return CalibrationPreview(chart.key, chart.label, chart.patch_count, used, matrix.tolist(), loss,
                              original_png, annotated_png, corrected_png,
                              frame.pixel_format, full_scale, provenance,
                              color_calibration=calibration, source_dir=directory)


FULL_RESOLUTION_KINDS = ("original", "annotated", "corrected")


def full_resolution_png(analysis: CalibrationPreview, kind: str) -> bytes:
    """Full-resolution 16-bit PNG for download, re-derived from the saved reference RAW."""
    if kind not in FULL_RESOLUTION_KINDS:
        raise ValueError(f"Unknown reference image kind: {kind!r}")
    if analysis.source_dir is None:
        raise ValueError("This calibration result has no saved reference RAW")
    provenance = analysis.provenance
    frame = recording.snapshot_frame(analysis.source_dir, verify_crc=True)
    if (frame.record.path.name != provenance["source_segment"]
            or frame.record.offset != provenance["source_record_offset"]):
        raise ValueError("The saved reference RAW no longer matches this calibration")
    rgb = native_rgb(frame)
    if kind == "original":
        return native_full_png16(rgb)
    if kind == "annotated":
        polygons = np.asarray(provenance["patch_polygons_native_pixels"], dtype=np.float64)
        return native_full_png16(_outline(rgb.copy(), polygons, (0, analysis.full_scale, 0)))
    normalization = CameraRGBNormalization.from_dict(provenance["normalization"])
    linear = normalize_camera_rgb(rgb, normalization) @ np.asarray(analysis.matrix, dtype=np.float32)
    return render.full_png16(linear)
