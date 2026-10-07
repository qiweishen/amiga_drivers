"""Offline snapshot metric checks; no device or capture process is started."""

import unittest

import cv2
import numpy as np

from gox_driver.scripts.jai_raw.pixels import prepare_snapshot_image as _prepare_snapshot_image


class SnapshotSaturationTests(unittest.TestCase):
    def test_saturated_red_or_blue_photosites_survive_brightness_conversion(self):
        for row, col in ((0, 0), (1, 1)):
            with self.subTest(bayer_site=(row, col)):
                raw = np.zeros((8, 8), dtype=np.uint16)
                raw[row::2, col::2] = 4095
                original = raw.copy()
                preview, clipped = _prepare_snapshot_image(raw, "BayerRG12Packed")
                self.assertEqual(clipped, 25.0)
                # A red-only/blue-only saturated scene is not white. Measuring
                # clipped% on this grayscale image would incorrectly yield zero.
                gray = cv2.cvtColor(preview, cv2.COLOR_BGR2GRAY)
                self.assertLess(int(gray.max()), 0xFFF0)
                np.testing.assert_array_equal(raw, original)

    def test_effective_depth_controls_saturation(self):
        for name, depth in (("Mono8", 8), ("Mono10", 10), ("Mono12p", 12), ("Mono16", 16)):
            with self.subTest(pixel_format=name):
                full_scale = (1 << depth) - 1
                dtype = np.uint8 if depth == 8 else np.uint16
                raw = np.array([[0, full_scale - 1], [full_scale, full_scale]], dtype=dtype)
                preview, clipped = _prepare_snapshot_image(raw, name)
                self.assertEqual(clipped, 50.0)
                self.assertEqual(preview.shape, raw.shape)

    def test_rgb_counts_native_components(self):
        raw = np.array([[[255, 0, 0], [0, 255, 0]]], dtype=np.uint8)
        preview, clipped = _prepare_snapshot_image(raw, "RGB8")
        self.assertAlmostEqual(clipped, 100.0 / 3.0)
        np.testing.assert_array_equal(preview, raw)

    def test_ambiguous_or_invalid_input_is_rejected(self):
        cases = (
            (np.zeros((2, 2), dtype=np.uint16), "Unknown12"),
            (np.zeros((2, 2), dtype=np.uint8), "BayerRG12Packed"),
            (np.full((2, 2), 4096, dtype=np.uint16), "Mono12"),
            (np.zeros((2, 2, 3), dtype=np.uint16), "BayerRG12"),
        )
        for raw, name in cases:
            with self.subTest(pixel_format=name, shape=raw.shape, dtype=raw.dtype):
                with self.assertRaises(ValueError):
                    _prepare_snapshot_image(raw, name)


if __name__ == "__main__":
    unittest.main()
