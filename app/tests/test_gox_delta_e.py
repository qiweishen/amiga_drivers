"""Per-patch CIEDE2000 against published reference pairs and OpenCV's fit loss; no camera."""

import unittest

import numpy as np

from app.decoder.gox import color_checker as cc
from app.decoder.gox import delta_e

# Sharma, Wu & Dalal (2005) test data: (L1, a1, b1), (L2, a2, b2), ΔE00.
_SHARMA = (
    ((50.0, 2.6772, -79.7751), (50.0, 0.0, -82.7485), 2.0425),
    ((50.0, 0.0, 0.0), (50.0, -1.0, 2.0), 2.3669),
    ((50.0, 2.5, 0.0), (73.0, 25.0, -18.0), 27.1492),
    ((60.2574, -34.0099, 36.2677), (60.4626, -34.1751, 39.4387), 1.2644),
    ((50.0, 2.5, 0.0), (50.0, 0.0, -2.5), 4.3065),
)


class DeltaETests(unittest.TestCase):
    def test_ciede2000_matches_the_published_reference_pairs(self):
        first = np.array([pair[0] for pair in _SHARMA])
        second = np.array([pair[1] for pair in _SHARMA])
        expected = np.array([pair[2] for pair in _SHARMA])
        np.testing.assert_allclose(delta_e.ciede2000(first, second), expected, atol=1e-4)
        np.testing.assert_allclose(delta_e.ciede2000(second, first), expected, atol=1e-4)

    def test_lab_of_white_and_black(self):
        lab = delta_e.lab_from_linear_srgb(np.array([[1.0, 1.0, 1.0], [0.0, 0.0, 0.0]]))
        np.testing.assert_allclose(lab, [[100.0, 0.0, 0.0], [0.0, 0.0, 0.0]], atol=1e-3)

    @unittest.skipUnless(cc.capability_reason() is None, "OpenCV ccm is unavailable")
    def test_rms_of_patch_errors_equals_the_opencv_fit_loss(self):
        rng = np.random.default_rng(7)
        chart = cc.CHARTS["MCC24"]
        mix = np.array([[0.30, 0.05, 0.02], [0.08, 0.42, 0.06], [0.01, 0.07, 0.25]])
        means = np.clip(rng.uniform(0.03, 0.9, (24, 3)) @ mix + rng.normal(0, 0.004, (24, 3)), 0.01, 0.9)
        matrix, loss, used, reference = cc._fit(means, chart)
        errors = cc._patch_errors(means, matrix, reference, chart)
        values = np.array([item["delta_e2000"] for item in errors])
        self.assertEqual(used, 24)
        self.assertAlmostEqual(float(np.sqrt(np.mean(values ** 2))), loss, places=4)
        self.assertEqual((errors[18]["patch"], errors[18]["name"]), (19, "White 9.5"))
        self.assertAlmostEqual(errors[18]["reference_lab"][0], 96.0, delta=1.5)


if __name__ == "__main__":
    unittest.main()
