"""Numerical and metadata contracts of the GOX colour-calibration values.

Synthetic arrays only: no camera, file writes, chart detection or fitting.
These tests do not establish calibration accuracy on a real chart.
"""

import unittest

import numpy as np

from app.decoder.gox.color import (
    CameraRGBNormalization,
    ColorCalibration,
    frame_exposure_from_device,
    normalize_camera_rgb,
    normalize_patch_means,
)

MEASURED = {"version": "pedestal_subtracted", "pedestal_status": "measured", "pedestal_dn": 128.0,
            "full_scale_dn": 4095}


def _device_json(*, integration=10002.45, gain="2.000000", serial="synthetic-camera", model="synthetic-model"):
    applied = [{"name": "ExposureTime", "requested": "10000", "readback": "10000.000000"}]
    if gain is not None:
        applied.append({"name": "Gain", "requested": "2", "readback": gain})
    return {"identity": {"DeviceSerialNumber": serial, "DeviceModelName": model},
            "derived": {"actual_integration_us": integration}, "applied": applied}


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
        result = normalize_camera_rgb(np.array([[[64, 128, 4095]]], dtype=np.uint16),
                                      CameraRGBNormalization.from_dict(MEASURED))
        np.testing.assert_allclose(result, [[[-64 / 3967, 0, 1]]], rtol=1e-7)
        self.assertLess(float(result[0, 0, 0]), 0)

    def test_patch_aggregation_retains_float64_precision_and_same_formula(self):
        means = np.array([[1000 + 1 / 49, 2000 + 2 / 49, 3000 + 3 / 49]], dtype=np.float64)
        legacy = normalize_patch_means(means, CameraRGBNormalization())
        self.assertEqual(legacy.dtype, np.dtype("float64"))
        np.testing.assert_array_equal(legacy, means / 4095)
        measured = normalize_patch_means(means, CameraRGBNormalization.from_dict(MEASURED))
        np.testing.assert_array_equal(measured, (means - 128) / 3967)

    def test_invalid_native_images_are_rejected(self):
        for raw in (np.zeros((1, 2, 3), dtype=np.float32),
                    np.zeros((1, 2, 4), dtype=np.uint16),
                    np.zeros((0, 2, 3), dtype=np.uint16)):
            with self.subTest(shape=raw.shape, dtype=raw.dtype), self.assertRaises(ValueError):
                normalize_camera_rgb(raw, CameraRGBNormalization())

    def test_unknown_and_inconsistent_normalization_are_rejected(self):
        base = CameraRGBNormalization().to_dict()
        changes = ({"version": "auto"}, {"full_scale_dn": 0},
                   {"pedestal_status": "measured"}, {"pedestal_dn": 128},
                   {"version": "pedestal_subtracted", "pedestal_dn": 128},
                   {**MEASURED, "pedestal_dn": 4095},
                   {**MEASURED, "pedestal_dn": float("nan")},
                   {**MEASURED, "pedestal_dn": [100.0, 110.0, 120.0, 130.0]})
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                CameraRGBNormalization.from_dict({**base, **change})

    def test_documented_scalar_pedestal_is_subtracted_without_clipping(self):
        policy = CameraRGBNormalization(version="pedestal_subtracted", pedestal_status="documented",
                                        pedestal_dn=128)
        raw = np.array([[[128, 100, 4095]]], dtype=np.uint16)
        np.testing.assert_allclose(normalize_camera_rgb(raw, policy)[0, 0], [0.0, -28 / 3967, 1.0], rtol=1e-6)
        self.assertEqual(CameraRGBNormalization.from_dict(policy.to_dict()), policy)


class CalibrationTests(unittest.TestCase):
    def calibration(self, matrix):
        return ColorCalibration("synthetic-reference-1", matrix, CameraRGBNormalization(),
                                applicability={"pixel_format": "BayerRG12Packed"},
                                fit={"chart_key": "MCC24", "used_patches": 24}, source={"sequence": 0})

    def test_document_keeps_the_matrix_and_metadata(self):
        document = self.calibration(np.eye(3) * 2).to_dict()
        self.assertEqual(list(document), ["schema_version", "calibration_id", "matrix", "matrix_convention",
                                          "normalization", "applicability", "fit", "source"])
        self.assertEqual(document["matrix"], (np.eye(3) * 2).tolist())
        self.assertEqual(document["matrix_convention"], "row_vector_camera_rgb @ matrix -> linear_srgb_d65")
        self.assertEqual(document["fit"], {"chart_key": "MCC24", "used_patches": 24})

    def test_singular_nonfinite_or_misshapen_matrices_are_rejected(self):
        for matrix in (np.zeros((3, 3)), np.diag([1.0, 1.0, 1e-9]), np.full((3, 3), np.nan), np.eye(2)):
            with self.subTest(matrix=matrix.tolist()), self.assertRaises(ValueError):
                self.calibration(matrix)


class FrameExposureTests(unittest.TestCase):
    def test_device_json_exposure_uses_actual_integration_and_gain_magnification(self):
        frame = frame_exposure_from_device(_device_json())
        self.assertEqual(frame.to_dict(), {"device_id": "synthetic-camera", "device_model": "synthetic-model",
                                           "exposure_time_us": 10002.45, "gain": 2.0})

    def test_device_json_without_readbacks_is_not_defaulted(self):
        for document in (_device_json(gain=None), _device_json(integration=None),
                         _device_json(gain="<failed>"), _device_json(gain="0"), _device_json(serial=None)):
            with self.subTest(document=document), self.assertRaises(ValueError):
                frame_exposure_from_device(document)


if __name__ == "__main__":
    unittest.main()
