"""Numerical and metadata contracts for reusable GOX colour transforms.

Synthetic arrays only: no camera, file writes, chart detection or fitting.
These tests do not establish calibration accuracy on a real chart.
"""

import copy
import json
import unittest

import numpy as np

from app.decoder.gox.color import (
    CameraRGBNormalization,
    ColorCalibration,
    FrameExposure,
    correct_linear,
    exposure_scale,
    frame_exposure_from_device,
    normalize_camera_rgb,
    normalize_patch_means,
)


def calibration_document():
    return {
        "schema_version": 1,
        "calibration_id": "synthetic-reference-1",
        "normalization": {
            "version": "dn_over_full_scale", "pedestal_status": "unknown",
            "pedestal_dn": None, "full_scale_dn": 4095,
        },
        "matrix": [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
        "matrix_convention": "row_vector_camera_rgb @ matrix -> linear_srgb_d65",
        "applicability": {
            "device_id": "synthetic-camera", "device_model": "synthetic-model",
            "pixel_format": "BayerRG12Packed", "exposure_time_us": 10000.0, "gain": 1.0,
            "raw_audit": {
                name: {"value": "Off", "outcome": "match", "expected": "Off"}
                for name in ("LUTMode", "GradationCompressionMode",
                             "ColorTransformationRGBMode", "BalanceWhiteAuto")
            },
            "exposure_compensation": "none",
        },
        "fit": {"chart_key": "MCC24", "loss_de2000_rms": None, "used_patches": 24},
        "source": {
            "session_id": "synthetic-session", "segment": "seg_00001.raw",
            "record_offset": 64, "sequence": 0, "block_id": 1,
        },
    }


class NormalizationTests(unittest.TestCase):
    def test_legacy_normalization_keeps_unknown_pedestal_and_native_codes(self):
        policy = CameraRGBNormalization()
        raw = np.array([[[1001, 2002, 3003], [0, 1, 4095]]], dtype=np.uint16)
        original = raw.copy()
        result = normalize_camera_rgb(raw, policy)
        self.assertEqual(result.dtype, np.dtype("float32"))
        np.testing.assert_array_equal(result, raw.astype(np.float32) / 4095)
        np.testing.assert_array_equal(raw, original)
        self.assertIsNone(policy.to_dict()["pedestal_dn"])
        self.assertEqual(policy.to_dict()["pedestal_status"], "unknown")

    def test_measured_pedestal_retains_negative_noise(self):
        policy = CameraRGBNormalization.from_dict({
            "version": "pedestal_subtracted", "pedestal_status": "measured",
            "pedestal_dn": 128.0, "full_scale_dn": 4095,
        })
        raw = np.array([[[64, 128, 4095]]], dtype=np.uint16)
        result = normalize_camera_rgb(raw, policy)
        np.testing.assert_allclose(result, [[[-64 / 3967, 0, 1]]], rtol=1e-7)
        self.assertLess(float(result[0, 0, 0]), 0)

    def test_patch_aggregation_retains_float64_precision_and_same_formula(self):
        means = np.array([[1000 + 1 / 49, 2000 + 2 / 49, 3000 + 3 / 49]], dtype=np.float64)
        legacy = normalize_patch_means(means, CameraRGBNormalization())
        self.assertEqual(legacy.dtype, np.dtype("float64"))
        np.testing.assert_array_equal(legacy, means / 4095)
        policy = CameraRGBNormalization.from_dict({
            "version": "pedestal_subtracted", "pedestal_status": "measured",
            "pedestal_dn": 128.0, "full_scale_dn": 4095,
        })
        measured = normalize_patch_means(means, policy)
        np.testing.assert_array_equal(measured, (means - 128) / 3967)

    def test_invalid_native_images_are_rejected_instead_of_clipped(self):
        for raw in (np.zeros((1, 2, 3), dtype=np.float32),
                    np.zeros((1, 2, 4), dtype=np.uint16),
                    np.zeros((0, 2, 3), dtype=np.uint16),
                    np.full((1, 2, 3), 4096, dtype=np.uint16)):
            with self.subTest(shape=raw.shape, dtype=raw.dtype), self.assertRaises(ValueError):
                normalize_camera_rgb(raw, CameraRGBNormalization())

    def test_unknown_and_inconsistent_normalization_are_rejected(self):
        base = CameraRGBNormalization().to_dict()
        changes = ({"version": "auto"}, {"full_scale_dn": 0},
                   {"pedestal_status": "measured"}, {"pedestal_dn": 128},
                   {"version": "pedestal_subtracted", "pedestal_dn": 128},
                   {"version": "pedestal_subtracted", "pedestal_status": "measured",
                    "pedestal_dn": 4095},
                   {"version": "pedestal_subtracted", "pedestal_status": "measured",
                    "pedestal_dn": float("nan")})
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                CameraRGBNormalization.from_dict({**base, **change})

    def test_cfa_pedestals_cannot_be_averaged_into_demosaiced_rgb(self):
        policy = CameraRGBNormalization.from_dict({
            "version": "pedestal_subtracted", "pedestal_status": "measured",
            "pedestal_dn": [100.0, 110.0, 120.0, 130.0], "full_scale_dn": 4095,
        })
        self.assertEqual(policy.to_dict()["pedestal_dn"], [100.0, 110.0, 120.0, 130.0])
        with self.assertRaises(ValueError):
            normalize_camera_rgb(np.full((2, 2, 3), 1000, dtype=np.uint16), policy)
        with self.assertRaises(ValueError):
            normalize_patch_means(np.full((24, 3), 1000.0), policy)

    def test_pedestal_without_float32_headroom_is_rejected_by_image_path(self):
        policy = CameraRGBNormalization.from_dict({
            "version": "pedestal_subtracted", "pedestal_status": "measured",
            "pedestal_dn": 4095.0 - 1e-8, "full_scale_dn": 4095,
        })
        with self.assertRaises(ValueError):
            normalize_camera_rgb(np.full((1, 1, 3), 4095, dtype=np.uint16), policy)
        np.testing.assert_array_equal(
            normalize_patch_means(np.full((1, 3), 4095.0), policy), np.ones((1, 3)))


class CalibrationTests(unittest.TestCase):
    def test_json_round_trip_preserves_unknown_values_and_zero_sequence(self):
        document = calibration_document()
        cal = ColorCalibration.from_dict(document)
        encoded = json.dumps(cal.to_dict(), allow_nan=False)
        reloaded = ColorCalibration.from_dict(json.loads(encoded))
        self.assertEqual(reloaded.to_dict(), document)

    def test_constructor_and_export_do_not_share_mutable_metadata_or_matrix(self):
        document = calibration_document()
        cal = ColorCalibration.from_dict(document)
        expected = copy.deepcopy(document)
        document["matrix"][0][0] = 20
        document["applicability"]["raw_audit"]["LUTMode"]["value"] = "On"
        exported = cal.to_dict()
        exported["matrix"][0][0] = 30
        exported["source"]["sequence"] = 99
        self.assertEqual(cal.to_dict(), expected)
        with self.assertRaises((TypeError, ValueError)):
            cal.matrix[0, 0] = 40

    def test_linear_output_retains_both_negative_and_above_one_values(self):
        document = calibration_document()
        document["normalization"].update(
            version="pedestal_subtracted", pedestal_status="measured", pedestal_dn=128.0)
        document["matrix"] = [[2.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 0.5]]
        cal = ColorCalibration.from_dict(document)
        raw = np.array([[[64, 128, 256], [4095, 4095, 4095]]], dtype=np.uint16)
        original = raw.copy()
        result = correct_linear(raw, cal)
        self.assertEqual(result.dtype, np.dtype("float32"))
        self.assertLess(float(result[0, 0, 0]), 0)
        self.assertEqual(float(result[0, 1, 0]), 2.0)
        np.testing.assert_array_equal(raw, original)

    def test_matrix_uses_row_vector_convention(self):
        document = calibration_document()
        document["matrix"] = [[1.0, 0.2, 0.1], [0.3, 1.0, 0.2], [0.1, 0.4, 1.0]]
        cal = ColorCalibration.from_dict(document)
        raw = np.array([[[1000, 2000, 3000]]], dtype=np.uint16)
        expected = np.array([[[1900, 3400, 3500]]], dtype=np.float32) / 4095
        np.testing.assert_allclose(correct_linear(raw, cal), expected, rtol=2e-7)

    def test_invalid_matrix_or_schema_is_rejected(self):
        for change in ({"schema_version": 2}, {"matrix_convention": "column_vector"},
                       {"matrix": [[1, 0], [0, 1]]}, {"matrix": [[0, 0, 0]] * 3},
                       {"matrix": [[1, True, 0], [0, 1, 0], [0, 0, 1]]},
                       {"matrix": [[float("inf"), 0, 0], [0, 1, 0], [0, 0, 1]]}):
            document = calibration_document()
            document.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                ColorCalibration.from_dict(document)
        document = calibration_document()
        document["fit"]["loss_de2000_rms"] = float("nan")
        with self.assertRaises(ValueError):
            ColorCalibration.from_dict(document)

    def test_incomplete_or_unverified_applicability_can_be_inspected_but_not_applied(self):
        for failure in ("device_id", "exposure_time_us", "gain", "raw_audit"):
            document = calibration_document()
            if failure == "raw_audit":
                document["applicability"]["raw_audit"]["LUTMode"]["outcome"] = "unreadable"
            else:
                document["applicability"][failure] = None
            cal = ColorCalibration.from_dict(document)
            self.assertEqual(cal.to_dict(), document)
            with self.subTest(failure=failure), self.assertRaises(ValueError):
                correct_linear(np.full((1, 1, 3), 1000, dtype=np.uint16), cal)

    def test_exposure_policy_cannot_silently_apply_without_frame_context(self):
        document = calibration_document()
        document["normalization"].update(
            version="pedestal_subtracted", pedestal_status="measured", pedestal_dn=128.0)
        document["applicability"]["exposure_compensation"] = "scale_by_exposure_ratio"
        cal = ColorCalibration.from_dict(document)
        with self.assertRaises(ValueError):
            correct_linear(np.full((2, 2, 3), 1000, dtype=np.uint16), cal)


def _device_json(*, integration=10002.45, gain="2.000000", serial="synthetic-camera",
                 model="synthetic-model"):
    applied = [{"name": "ExposureTime", "requested": "10000", "readback": "10000.000000"}]
    if gain is not None:
        applied.append({"name": "Gain", "requested": "2", "readback": gain})
    return {"identity": {"DeviceSerialNumber": serial, "DeviceModelName": model},
            "derived": {"actual_integration_us": integration}, "applied": applied}


def _ratio_calibration(*, pedestal_status="none"):
    document = calibration_document()
    document["normalization"]["pedestal_status"] = pedestal_status
    document["applicability"].update(exposure_time_us=10002.45, gain=2.0,
                                     exposure_compensation="scale_by_exposure_ratio")
    return ColorCalibration.from_dict(document)


class ExposureCompensationTests(unittest.TestCase):
    def test_device_json_exposure_uses_actual_integration_and_gain_magnification(self):
        frame = frame_exposure_from_device(_device_json())
        self.assertEqual(frame.to_dict(), {"device_id": "synthetic-camera", "device_model": "synthetic-model",
                                           "exposure_time_us": 10002.45, "gain": 2.0})

    def test_device_json_without_readbacks_is_not_defaulted(self):
        for document in (_device_json(gain=None), _device_json(integration=None),
                         _device_json(gain="<failed>"), _device_json(gain="0"), _device_json(serial=None)):
            with self.subTest(document=document), self.assertRaises(ValueError):
                frame_exposure_from_device(document)

    def test_zero_black_level_decision_permits_ratio_policy_but_unknown_does_not(self):
        self.assertEqual(_ratio_calibration().normalization.pedestal_status, "none")
        with self.assertRaisesRegex(ValueError, "black level"):
            _ratio_calibration(pedestal_status="unknown")

    def test_ratio_policy_scales_normalized_rgb_by_exposure_times_gain(self):
        cal = _ratio_calibration()
        raw = np.full((1, 2, 3), 1000, dtype=np.uint16)
        # Half the integration and half the gain: four times less signal.
        frame = FrameExposure("synthetic-camera", "synthetic-model", 10002.45 / 2, 1.0)
        self.assertAlmostEqual(exposure_scale(cal, frame), 4.0)
        np.testing.assert_allclose(correct_linear(raw, cal, frame), 4000 / 4095, rtol=1e-6)
        same = FrameExposure("synthetic-camera", "synthetic-model", 10002.45, 2.0)
        np.testing.assert_allclose(correct_linear(raw, cal, same), 1000 / 4095, rtol=1e-6)

    def test_ratio_policy_requires_frame_and_matching_device(self):
        cal = _ratio_calibration()
        raw = np.full((1, 1, 3), 1000, dtype=np.uint16)
        with self.assertRaisesRegex(ValueError, "FrameExposure"):
            correct_linear(raw, cal)
        for frame in (FrameExposure("other-camera", "synthetic-model", 100.0, 1.0),
                      FrameExposure("synthetic-camera", "other-model", 100.0, 1.0)):
            with self.subTest(frame=frame), self.assertRaisesRegex(ValueError, "does not match"):
                correct_linear(raw, cal, frame)

    def test_none_policy_rejects_a_frame_at_a_different_exposure(self):
        cal = ColorCalibration.from_dict(calibration_document())
        raw = np.full((1, 1, 3), 1000, dtype=np.uint16)
        matching = FrameExposure("synthetic-camera", "synthetic-model", 10000.0, 1.0)
        np.testing.assert_allclose(correct_linear(raw, cal, matching), 1000 / 4095, rtol=1e-6)
        with self.assertRaisesRegex(ValueError, "policy is 'none'"):
            correct_linear(raw, cal, FrameExposure("synthetic-camera", "synthetic-model", 20000.0, 1.0))



class DocumentedPedestalTests(unittest.TestCase):
    def test_documented_scalar_pedestal_is_subtracted_without_clipping(self):
        policy = CameraRGBNormalization(version="pedestal_subtracted", pedestal_status="documented",
                                        pedestal_dn=128)
        raw = np.array([[[128, 100, 4095]]], dtype=np.uint16)
        np.testing.assert_allclose(normalize_camera_rgb(raw, policy)[0, 0], [0.0, -28 / 3967, 1.0], rtol=1e-6)
        self.assertEqual(CameraRGBNormalization.from_dict(policy.to_dict()), policy)

    def test_documented_pedestal_is_one_scalar(self):
        with self.assertRaisesRegex(ValueError, "scalar"):
            CameraRGBNormalization(version="pedestal_subtracted", pedestal_status="documented",
                                   pedestal_dn=(128, 128, 128, 128))


if __name__ == "__main__":
    unittest.main()
