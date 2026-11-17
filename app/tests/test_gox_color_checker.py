"""ColorChecker native DN contracts; mocked chart detection/CCM fitting, no camera.

These tests exercise native sample handling and rejected calibration results.
They do not establish detector accuracy or calibration accuracy on real charts.
"""

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import numpy as np

from app.decoder.gox import color_checker as cc
from app.decoder.gox import preview
from app.tests.test_gox_format import write_gox_fixture


def _measurements(count):
    # Three independent RGB directions, safely away from full scale.
    basis = np.array([[0.4, 0.1, 0.2], [0.2, 0.5, 0.1], [0.1, 0.2, 0.6]], dtype=np.float64)
    return basis[np.arange(count) % 3].copy()


def _ccm_stub(count, *, matrix=None, loss=1.25, mask=None):
    model = Mock()
    model.getColorCorrectionMatrix.return_value = np.eye(3) if matrix is None else matrix
    model.getLoss.return_value = loss
    model.getMask.return_value = np.ones((count, 1), dtype=np.uint8) if mask is None else mask
    model.getRefLinearRGB.return_value = np.full((count, 1, 3), 0.2)
    api = SimpleNamespace(
        COLORCHECKER_MACBETH=101, COLORCHECKER_DIGITAL_SG=202, COLORCHECKER_VINYL=303,
        COLOR_SPACE_SRGB=401, CCM_LINEAR=402, LINEARIZATION_IDENTITY=403, DISTANCE_CIE2000=404,
        ColorCorrectionModel=Mock(return_value=model),
    )
    return api, model


def _checker(target, polygons, cost=0.05):
    checker = Mock()
    checker.getTarget.return_value = target
    checker.getColorCharts.return_value = polygons.reshape(-1, 2)
    checker.getCost.return_value = cost
    return checker


def _sample(rgb, raw, polygons, full_scale):
    """Patch means for fitting; a saturated patch raises, as in calibrate()."""
    means, counts, status = cc._sample_patches(rgb, raw, polygons, full_scale)
    cc._require_unsaturated(status)
    return means, counts


def _mcc_stub(key, polygons):
    constants = {"MCC24": 11, "SG140": 22, "VINYL18": 33}
    checker = _checker(constants[key], polygons)
    detector = Mock()
    detector.process.return_value = True
    detector.getListColorChecker.return_value = [checker]
    api = SimpleNamespace(**constants, CCheckerDetector=SimpleNamespace(create=Mock(return_value=detector)))
    return api, detector, checker


def _reference_frame(raw):
    return SimpleNamespace(
        image=raw, pixel_format="BayerRG12Packed", skipped_invalid=False,
        record=SimpleNamespace(path=Path("reference/seg_00001.raw"), offset=64,
                               sequence=0, bid=1, hrt=1800000000000000001),
    )


def _valid_audit():
    return {name: {"value": "Off", "expected": "Off", "outcome": "match"}
            for name in ("LUTMode", "GradationCompressionMode", "ColorTransformationRGBMode", "BalanceWhiteAuto")}


class GoxColorCheckerTests(unittest.TestCase):
    def test_each_chart_uses_temporary_8_bit_detection_without_changing_native_rgb(self):
        for key in ("MCC24", "SG140", "VINYL18"):
            with self.subTest(chart=key):
                chart = cc.CHARTS[key]
                polygons = np.tile([[2, 2], [8, 2], [8, 8], [2, 8]],
                                   (chart.patch_count, 1, 1)).astype(np.float64)
                native = np.broadcast_to(np.array([1001, 2002, 3003], dtype=np.uint16),
                                         (12, 12, 3)).copy()
                original = native.copy()
                display = cc._display_rgb(native)
                display_before = display.copy()
                mcc, detector, _ = _mcc_stub(key, polygons)
                with patch.object(cc.cv, "mcc", mcc, create=True):
                    candidates, transfer = cc._detect(display, chart)
                detector.setColorChartType.assert_called_once_with(getattr(mcc, key))
                detection_image, max_candidates = detector.process.call_args.args
                self.assertEqual(detection_image.dtype, np.dtype("uint8"))
                self.assertEqual(detection_image.shape, native.shape)
                # An upper bound on OpenCV's candidates, not a count of charts.
                self.assertEqual(max_candidates, cc._MAX_CANDIDATES)
                expected_rgb8 = np.rint(cc.render.srgb_oetf(display) * 255).astype(np.uint8)
                np.testing.assert_array_equal(detection_image, expected_rgb8[..., ::-1])
                self.assertEqual(len(candidates), 1)
                np.testing.assert_array_equal(candidates[0].polygons, polygons)
                self.assertEqual(candidates[0].cost, 0.05)
                np.testing.assert_array_equal(display, display_before)
                np.testing.assert_array_equal(native, original)
                self.assertIn("geometry only", transfer)

    def test_each_chart_uses_its_reference_colors_and_linear_native_measurements(self):
        for key, count, reference in (("MCC24", 24, 101), ("SG140", 140, 202), ("VINYL18", 18, 303)):
            with self.subTest(chart=key):
                chart = cc.CHARTS[key]
                self.assertEqual(chart.patch_count, count)
                ccm, model = _ccm_stub(count)
                means = _measurements(count)
                with patch.object(cc.cv, "ccm", ccm, create=True):
                    matrix, loss, used, reference_linear = cc._fit(means, chart)
                self.assertEqual(reference_linear.shape, (count, 3))
                submitted, actual_reference = ccm.ColorCorrectionModel.call_args.args
                self.assertEqual(actual_reference, reference)
                self.assertEqual(submitted.shape, (count, 1, 3))
                np.testing.assert_array_equal(submitted[:, 0], means)
                model.setLinearization.assert_called_once_with(ccm.LINEARIZATION_IDENTITY)
                model.setCcmType.assert_called_once_with(ccm.CCM_LINEAR)
                model.setColorSpace.assert_called_once_with(ccm.COLOR_SPACE_SRGB)
                model.setDistance.assert_called_once_with(ccm.DISTANCE_CIE2000)
                self.assertEqual((loss, used), (1.25, count))
                np.testing.assert_array_equal(matrix, np.eye(3))

    def test_calibration_rejects_non_bayerrg12packed_images(self):
        for name, native in (("RGB8", np.zeros((8, 8, 3), dtype=np.uint8)),
                             ("BayerRG12", np.zeros((8, 8), dtype=np.uint16)),
                             ("BayerRG12p", np.zeros((8, 8), dtype=np.uint16)),
                             ("Mono12", np.zeros((8, 8), dtype=np.uint16))):
            with self.subTest(pixel_format=name):
                frame = SimpleNamespace(image=native, pixel_format=name)
                with self.assertRaisesRegex(ValueError, "GOX requires BayerRG12Packed"):
                    cc.native_rgb(frame)

    def test_bayer_demosaic_keeps_native_12_bit_values_and_converts_bgr_to_rgb(self):
        native = np.full((8, 8), 1001, dtype=np.uint16)
        bgr = np.broadcast_to(np.array([103, 205, 307], dtype=np.uint16), (8, 8, 3)).copy()
        frame = SimpleNamespace(image=native, pixel_format="BayerRG12Packed")
        with patch.object(preview.pixels, "demosaic", return_value=bgr) as demosaic:
            rgb = cc.native_rgb(frame)
        supplied, pattern = demosaic.call_args.args
        self.assertIs(supplied, native)  # No shift-to-16 or 8-bit intermediate.
        self.assertEqual(pattern, "RG")
        self.assertEqual(rgb.dtype, np.dtype("uint16"))
        np.testing.assert_array_equal(rgb[3, 3], [307, 205, 103])
        np.testing.assert_array_equal(native, np.full((8, 8), 1001, dtype=np.uint16))

    def test_sampling_keeps_dn_steps_smaller_than_an_8_bit_display_step(self):
        rgb = np.broadcast_to(np.array([1001, 2002, 3003], dtype=np.uint16), (10, 12, 3)).copy()
        raw = np.full((10, 12), 1001, dtype=np.uint16)
        polygon = np.array([[[2, 2], [8, 2], [8, 6], [2, 6]]], dtype=np.float64)
        means, counts = _sample(rgb, raw, polygon, 4095)
        np.testing.assert_allclose(means[0] * 4095, [1001, 2002, 3003], rtol=0, atol=1e-10)
        self.assertEqual(counts, [35])
        rgb[:, :, 0] += 1
        shifted, _ = _sample(rgb, raw, polygon, 4095)
        self.assertAlmostEqual((shifted[0, 0] - means[0, 0]) * 4095, 1.0)

    def test_vinyl_sampling_uses_provided_unequal_rectangles_in_reference_order(self):
        rgb = np.zeros((24, 112, 3), dtype=np.uint16)
        polygons, expected = [], []
        for index in range(18):
            # Twelve squares and six wider rectangles; deliberately no 6x3 grid.
            x, y, width = (2 + 9 * index, 2, 5) if index < 12 else (2 + 16 * (index - 12), 14, 12)
            polygon = [[x, y], [x + width, y], [x + width, y + 5], [x, y + 5]]
            value = [100 + 3 * index, 200 + 5 * index, 300 + 7 * index]
            rgb[y:y + 6, x:x + width + 1] = value
            polygons.append(polygon)
            expected.append(value)
        polygons = np.asarray(polygons, dtype=np.float64)
        means, counts = _sample(rgb, rgb, polygons, 4095)
        np.testing.assert_allclose(means * 4095, expected, rtol=0, atol=1e-10)
        self.assertEqual(counts, [36] * 12 + [78] * 6)

    def test_sampling_rejects_outside_tiny_and_nonconvex_polygons(self):
        image = np.full((12, 12, 3), 100, dtype=np.uint16)
        cases = (
            [[-1, 2], [6, 2], [6, 8], [-1, 8]],
            [[2, 2], [12, 2], [12, 8], [2, 8]],
            [[2, 2], [3, 2], [3, 3], [2, 3]],
            [[2, 2], [9, 2], [3, 3], [2, 9]],
        )
        for polygon in cases:
            with self.subTest(polygon=polygon), self.assertRaises(ValueError):
                _sample(image, image, np.array([polygon], dtype=np.float64), 4095)

    def test_saturated_native_photosite_is_rejected_even_if_demosaic_conceals_it(self):
        rgb = np.full((12, 12, 3), 1000, dtype=np.uint16)
        raw = np.full((12, 12), 1000, dtype=np.uint16)
        raw[4, 4] = 4095
        polygon = np.array([[[2, 2], [8, 2], [8, 8], [2, 8]]], dtype=np.float64)
        with self.assertRaisesRegex(ValueError, "saturated"):
            _sample(rgb, raw, polygon, 4095)
        almost_full = np.full((12, 12, 3), 4090, dtype=np.uint16)
        with self.assertRaisesRegex(ValueError, "close to saturation"):
            _sample(almost_full, almost_full, polygon, 4095)

    def test_detection_rejects_wrong_patch_count_nonfinite_geometry_and_wrong_chart_type(self):
        chart = cc.CHARTS["MCC24"]
        polygons = np.tile([[2, 2], [8, 2], [8, 8], [2, 8]], (24, 1, 1)).astype(np.float64)
        for case in ("short", "nonfinite", "wrong_type", "missing"):
            with self.subTest(case=case):
                mcc, detector, checker = _mcc_stub("MCC24", polygons)
                if case == "short":
                    checker.getColorCharts.return_value = polygons[:-1]
                elif case == "nonfinite":
                    bad = polygons.copy()
                    bad[0, 0, 0] = np.nan
                    checker.getColorCharts.return_value = bad
                elif case == "wrong_type":
                    checker.getTarget.return_value = mcc.VINYL18
                else:
                    detector.process.return_value = False
                with patch.object(cc.cv, "mcc", mcc, create=True), self.assertRaises(ValueError):
                    cc._detect(np.full((12, 12, 3), 0.25, dtype=np.float32), chart)

    def test_detection_returns_every_complete_candidate_in_opencv_order(self):
        chart = cc.CHARTS["MCC24"]
        polygons = np.tile([[2, 2], [8, 2], [8, 8], [2, 8]], (24, 1, 1)).astype(np.float64)
        mcc, detector, _ = _mcc_stub("MCC24", polygons)
        incomplete = _checker(mcc.MCC24, polygons[:-1], cost=0.01)
        detector.getListColorChecker.return_value = [
            incomplete, _checker(mcc.MCC24, polygons, cost=0.02), _checker(mcc.MCC24, polygons + 20, cost=0.03)]
        with patch.object(cc.cv, "mcc", mcc, create=True):
            candidates, _ = cc._detect(np.full((12, 12, 3), 0.25, dtype=np.float32), chart)
        # Two returned checkers are not two charts: _select_chart decides on native measurements.
        self.assertEqual([candidate.cost for candidate in candidates], [0.02, 0.03])
        np.testing.assert_array_equal(candidates[1].polygons, polygons + 20)

    def test_detect_offers_every_candidate_in_native_pixels_with_its_own_thumbnail(self):
        raw = np.full((12, 12), 1001, dtype=np.uint16)
        rgb = np.broadcast_to(np.array([1001, 2002, 3003], dtype=np.uint16), (12, 12, 3)).copy()
        original_rgb = rgb.copy()
        first = np.tile([[2, 2], [8, 2], [8, 8], [2, 8]], (24, 1, 1)).astype(np.float64)
        second = np.tile([[1, 1], [4, 1], [4, 4], [1, 4]], (24, 1, 1)).astype(np.float64)
        # Halving detector resolution moves its pixel centers by half a pixel.
        found = [cc._Candidate((polygons + 0.5) / 2 - 0.5, cost) for polygons, cost in ((first, 0.02), (second, 0.03))]
        with patch.object(cc, "capability_reason", return_value=None), \
                patch.object(cc, "_linear_audit", return_value=_valid_audit()), \
                patch.object(cc.recording, "snapshot_frame", return_value=_reference_frame(raw)) as reader, \
                patch.object(cc, "native_rgb", return_value=rgb), \
                patch.object(cc, "_DETECTION_EDGE", 6), \
                patch.object(cc, "_detect", return_value=(found, "test transfer")) as detect:
            detection = cc.detect(Path("reference"), "MCC24")
        reader.assert_called_once_with(Path("reference"), verify_crc=True)
        supplied_display, supplied_chart = detect.call_args.args
        self.assertEqual((supplied_display.dtype, supplied_display.shape), (np.dtype("float32"), (6, 6, 3)))
        self.assertEqual(supplied_chart.key, "MCC24")
        self.assertEqual((detection.chart_key, detection.transfer), ("MCC24", "test transfer"))
        # Every candidate is offered, numbered in OpenCV's order; nothing is chosen here.
        self.assertEqual([(item.number, item.cost) for item in detection.candidates], [(1, 0.02), (2, 0.03)])
        np.testing.assert_allclose(detection.candidates[0].polygons, first, rtol=0, atol=1e-12)
        np.testing.assert_allclose(detection.candidates[1].polygons, second, rtol=0, atol=1e-12)
        thumbnails = []
        for candidate in detection.candidates:
            self.assertEqual(candidate.thumbnail_png[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(candidate.thumbnail_png[24], 8)  # an 8-bit screen product
            thumbnails.append(cc.cv.imdecode(np.frombuffer(candidate.thumbnail_png, np.uint8),
                                             cc.cv.IMREAD_UNCHANGED))
        self.assertEqual(thumbnails[0].shape, rgb.shape)  # 12 px is below the thumbnail limit
        # A magenta outline (B, G, R = 255, 0, 255) marks where each candidate lies.
        np.testing.assert_array_equal(thumbnails[0][2, 5], [255, 0, 255])
        self.assertFalse(np.array_equal(thumbnails[0], thumbnails[1]))
        self.assertEqual(detection.candidates[0].to_dict()["candidate"], 1)
        np.testing.assert_array_equal(rgb, original_rgb)

    def test_measure_samples_only_the_selected_geometry(self):
        raw = np.zeros((12, 12), dtype=np.uint16)
        rgb = np.broadcast_to(np.array([1001, 2002, 3003], dtype=np.uint16), (12, 12, 3)).copy()
        polygons = np.tile([[2, 2], [8, 2], [8, 8], [2, 8]], (24, 1, 1)).astype(np.float64)
        selection = {"candidate": 2, "selected_by": "operator"}
        with patch.object(cc, "capability_reason", return_value=None), \
                patch.object(cc, "_linear_audit", return_value=_valid_audit()), \
                patch.object(cc.recording, "snapshot_frame", return_value=_reference_frame(raw)), \
                patch.object(cc, "native_rgb", return_value=rgb), \
                patch.object(cc, "_detect") as detect:
            measurement = cc.measure(Path("reference"), "MCC24", polygons, selection=selection)
            with self.assertRaisesRegex(ValueError, "does not match the selected chart format"):
                cc.measure(Path("reference"), "MCC24", polygons[:-1])
        detect.assert_not_called()  # measuring never re-detects
        self.assertEqual((measurement.geometry, measurement.detection_transfer, measurement.selection),
                         (cc.GEOMETRY_SUPPLIED, cc.SUPPLIED_TRANSFER, selection))
        np.testing.assert_allclose(measurement.means * 4095, np.tile([1001, 2002, 3003], (24, 1)),
                                   rtol=0, atol=1e-9)
        self.assertEqual(measurement.counts, [49] * 24)

    def test_rank_deficient_or_nonfinite_measurements_never_start_model_fitting(self):
        cases = [np.ones((24, 3)) * 0.2, np.full((24, 3), np.nan)]
        for means in cases:
            ccm, _ = _ccm_stub(24)
            with patch.object(cc.cv, "ccm", ccm, create=True), self.assertRaises(ValueError):
                cc._fit(means, cc.CHARTS["MCC24"])
            ccm.ColorCorrectionModel.assert_not_called()

    def test_invalid_fit_results_are_rejected(self):
        cases = (
            {"matrix": np.zeros((3, 3))},
            {"matrix": np.ones((4, 3))},
            {"matrix": np.full((3, 3), np.nan)},
            {"matrix": np.diag([1.0, 1.0, 1e-8])},
            {"loss": float("nan")}, {"loss": float("inf")}, {"loss": -1.0},
            {"mask": np.r_[0, np.ones(23)]}, {"mask": np.ones(23)},
        )
        for index, kwargs in enumerate(cases):
            with self.subTest(case=index):
                ccm, _ = _ccm_stub(24, **kwargs)
                with patch.object(cc.cv, "ccm", ccm, create=True), self.assertRaises(ValueError):
                    cc._fit(_measurements(24), cc.CHARTS["MCC24"])

    def test_missing_unreadable_or_enabled_linear_audit_is_rejected(self):
        for feature in _valid_audit():
            for failure in ("missing", "unreadable", "deviates", "enabled"):
                with self.subTest(feature=feature, failure=failure):
                    audit = _valid_audit()
                    if failure == "missing":
                        del audit[feature]
                    elif failure == "enabled":
                        audit[feature]["value"] = "On"
                    else:
                        audit[feature]["outcome"] = failure
                    with patch.object(cc.raw_recording, "device_metadata", return_value={"raw_audit": audit}):
                        with self.assertRaisesRegex(ValueError, feature):
                            cc._linear_audit(Path("reference"))
        audit = _valid_audit()
        with patch.object(cc.raw_recording, "device_metadata", return_value={"raw_audit": audit}):
            self.assertEqual(cc._linear_audit(Path("reference")), audit)

    def test_chart_selection_is_required_before_reading_a_capture(self):
        polygons = np.zeros((24, 4, 2))
        for key in (None, "", "mcc24", "AUTO", 0):
            with self.subTest(key=key), patch.object(cc.recording, "snapshot_frame") as reader:
                with self.assertRaisesRegex(ValueError, "Select"):
                    cc.detect(Path("reference"), key)
                with self.assertRaisesRegex(ValueError, "Select"):
                    cc.measure(Path("reference"), key, polygons)
                reader.assert_not_called()

    def test_selected_geometry_is_sampled_natively_and_full_resolution_png_previews_are_lossless(self):
        raw = np.full((12, 12), 1001, dtype=np.uint16)
        original_raw = raw.copy()
        rgb = np.broadcast_to(np.array([1001, 2002, 3003], dtype=np.uint16), (12, 12, 3)).copy()
        original_rgb = rgb.copy()
        polygons = np.tile([[2, 2], [8, 2], [8, 8], [2, 8]], (24, 1, 1)).astype(np.float64)
        frame = _reference_frame(raw)
        detection_transfer = "sRGB-like OETF; used for geometry only"
        selection = {"candidate": 1, "opencv_cost": 0.05, "candidate_count": 1, "selected_by": "operator"}
        exposure = cc.FrameExposure("SN-1", "synthetic-model", 10002.45, 2.0)
        with patch.object(cc, "capability_reason", return_value=None), \
                patch.object(cc, "_linear_audit", return_value=_valid_audit()), \
                patch.object(cc.recording, "snapshot_frame", return_value=frame) as reader, \
                patch.object(cc, "native_rgb", return_value=rgb), \
                patch.object(cc, "_fit", return_value=(np.eye(3), 0.5, 24, np.full((24, 3), 0.2))) as fit, \
                patch.object(cc, "_reference_exposure", return_value=(exposure, "")):
            measurement = cc.measure(Path("reference"), "MCC24", polygons, geometry=cc.GEOMETRY_SELECTED,
                                     detection_transfer=detection_transfer, selection=selection)
            result = cc.calibrate(Path("reference"), "MCC24", measurement=measurement)
            with self.assertRaisesRegex(ValueError, "another capture or chart"):
                cc.calibrate(Path("reference"), "SG140", measurement=measurement)
        np.testing.assert_allclose(result.provenance["patch_polygons_native_pixels"], polygons, rtol=0, atol=1e-12)
        self.assertEqual(result.provenance["chart_geometry"], cc.GEOMETRY_SELECTED)
        self.assertEqual(result.provenance["chart_selection"], selection)
        reader.assert_called_once_with(Path("reference"), verify_crc=True)
        measured, chart = fit.call_args.args
        self.assertEqual(chart.key, "MCC24")
        self.assertEqual(measured.dtype, np.dtype("float64"))
        # The fit sees the documented 128 DN pedestal removed: (DN - 128) / (4095 - 128).
        np.testing.assert_allclose(measured * (4095 - 128) + 128, np.tile([1001, 2002, 3003], (24, 1)),
                                   rtol=0, atol=1e-9)
        for name in ("original_png", "annotated_png", "corrected_png"):
            encoded = getattr(result, name)
            self.assertEqual(encoded[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(encoded[24], 8)  # on-screen previews are 8-bit display products
            decoded = cc.cv.imdecode(np.frombuffer(encoded, np.uint8), cc.cv.IMREAD_UNCHANGED)
            self.assertEqual(decoded.dtype, np.dtype("uint8"))
            self.assertEqual(decoded.shape, rgb.shape)  # 12 px is below the display limit
            if name == "original_png":
                np.testing.assert_array_equal(decoded[..., ::-1],
                                              np.rint(original_rgb / 4095 * 255).astype(np.uint8))
        # Full-resolution downloads are re-derived from the saved RAW, losslessly.
        with patch.object(cc.recording, "snapshot_frame", return_value=frame), \
                patch.object(cc, "native_rgb", return_value=rgb):
            full = {kind: cc.full_resolution_png(result, kind) for kind in cc.FULL_RESOLUTION_KINDS}
        for kind, encoded in full.items():
            self.assertEqual(encoded[24], 16, kind)
            decoded = cc.cv.imdecode(np.frombuffer(encoded, np.uint8), cc.cv.IMREAD_UNCHANGED)
            self.assertEqual(decoded.shape, rgb.shape)
            if kind == "original":
                np.testing.assert_array_equal(decoded[..., ::-1], original_rgb << 4)
        with self.assertRaisesRegex(ValueError, "Unknown"):
            cc.full_resolution_png(result, "raw")
        self.assertEqual(result.full_scale, 4095)
        self.assertEqual(result.provenance["source_effective_bits"], 12)
        self.assertEqual(result.provenance["normalization"], {
            "version": "pedestal_subtracted", "pedestal_status": "documented",
            "pedestal_dn": 128.0, "full_scale_dn": 4095,
        })
        black = result.provenance["black_level"]
        self.assertEqual((black["pedestal_dn"], black["status"]), (128.0, "documented"))
        self.assertIn("p.172", black["source"])
        self.assertIn("8LSB@8bit", black["source"])
        self.assertEqual(result.provenance["reference_exposure"], exposure.to_dict())
        reusable = result.color_calibration
        self.assertEqual(reusable["applicability"]["exposure_compensation"], "scale_by_exposure_ratio")
        self.assertEqual(reusable["applicability"]["gain"], 2.0)
        self.assertEqual(reusable["applicability"]["exposure_time_us"], 10002.45)
        self.assertEqual((reusable["normalization"]["pedestal_status"], reusable["normalization"]["pedestal_dn"]),
                         ("documented", 128.0))
        self.assertEqual(reusable["source"]["block_id"], 1)
        display = result.provenance["display"]
        self.assertEqual(display, cc.render.PROVENANCE)
        self.assertEqual(display["exposure_adjustment"], "none")
        # Identity matrix: the brightest patch maps to its pedestal-subtracted input, so the
        # gain is luminance((DN - 128) / 3967) divided by its peak (3003 - 128) / 3967.
        diagnostics = result.provenance["exposure_diagnostics"]
        expected_luminance = float((np.array([1001, 2002, 3003]) - 128) @ [0.2126, 0.7152, 0.0722]) / 3967
        self.assertAlmostEqual(diagnostics["brightest_patch_linear_srgb_luminance"], expected_luminance)
        self.assertAlmostEqual(diagnostics["chart_exposure_gain"], expected_luminance / ((3003 - 128) / 3967))
        self.assertAlmostEqual(diagnostics["brightest_patch_peak_dn"], 3003.0)
        self.assertNotIn("ev_to_cancel_chart_gain", diagnostics)
        errors = result.provenance["patch_errors"]
        self.assertEqual([item["patch"] for item in errors], list(range(1, 25)))
        self.assertEqual(errors[0]["name"], "Dark skin")
        self.assertTrue(all(item["delta_e2000"] >= 0 for item in errors))
        self.assertEqual(result.source_dir, Path("reference"))
        self.assertEqual(result.provenance["detection_transfer"], detection_transfer)
        self.assertEqual(result.provenance["patch_polygons_native_pixels"], polygons.tolist())
        self.assertEqual(result.provenance["patch_pixel_counts"], [49] * 24)
        np.testing.assert_array_equal(raw, original_raw)
        np.testing.assert_array_equal(rgb, original_rgb)

    def test_corrupt_reference_payload_is_rejected_before_detection_or_measurement(self):
        with tempfile.TemporaryDirectory() as temporary:
            camera = Path(temporary) / "camera"
            write_gox_fixture(camera)
            frame = cc.recording.snapshot_frame(camera, verify_crc=True)
            segment = frame.record.path
            damaged = bytearray(segment.read_bytes())
            damaged[frame.record.payload_offset + 2] ^= 1
            segment.write_bytes(damaged)
            with patch.object(cc, "capability_reason", return_value=None), \
                    patch.object(cc, "_linear_audit", return_value=_valid_audit()), \
                    patch.object(cc, "_detect") as detect, \
                    patch.object(cc, "_sample_patches") as sample:
                with self.assertRaisesRegex(ValueError, "payload CRC-32C mismatch"):
                    cc.detect(camera, "MCC24")
                with self.assertRaisesRegex(ValueError, "payload CRC-32C mismatch"):
                    cc.measure(camera, "MCC24", np.zeros((24, 4, 2)))
                detect.assert_not_called()
                sample.assert_not_called()
            self.assertEqual(segment.read_bytes(), damaged)



class DocumentedBlackLevelTests(unittest.TestCase):
    def _black(self, document):
        with patch.object(cc.raw_recording, "device_metadata", return_value=document):
            return cc._reference_black_level(Path("reference"))

    def test_documented_pedestal_matches_device_json(self):
        black = self._black({"derived": {"black_level": {
            "black_level_register": "0", "documented_pedestal": "8LSB@8bit", "pedestal_lsb8": 8,
            "pedestal_lsb12_derived": 128, "derived_by_driver": True}}})
        self.assertEqual((black["pedestal_dn"], black["status"], black["warnings"]), (128.0, "documented", []))
        self.assertEqual(black["device_json"]["documented_pedestal"], "8LSB@8bit")

    def test_disagreements_are_reported_not_hidden(self):
        black = self._black({"derived": {"black_level": {"black_level_register": "12",
                                                         "pedestal_lsb12_derived": 128}}})
        self.assertEqual(black["pedestal_dn"], 128.0)
        self.assertTrue(any("BlackLevel register" in warning for warning in black["warnings"]))
        self.assertTrue(self._black({})["warnings"])


if __name__ == "__main__":
    unittest.main()
