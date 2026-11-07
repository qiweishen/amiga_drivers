"""ETTR exposure arithmetic on synthetic linear chart measurements; no camera."""

import math
import unittest

import numpy as np

from app.decoder.gox import exposure

OFFSET_US = 2.45
BLACK_DN = 145.0
SLOPE = 0.0081  # DN per µs at gain x1 (≈551 DN at 50 ms, as measured in the field)


def _point(ms, *, gain=1.0, status=None, black=BLACK_DN, slope=SLOPE, label=None):
    t_us = ms * 1000.0 + OFFSET_US
    return exposure.ExposurePoint(label or f"{ms:g} ms", t_us, gain, 19,
                                  min(4095.0, black + slope * t_us * gain), "G", status, ms)


class BracketTests(unittest.TestCase):
    def test_bracket_is_center_plus_minus_50_ms_or_half_center_below(self):
        self.assertEqual(exposure.bracket(120), [70.0, 120.0, 170.0])
        self.assertEqual(exposure.bracket(50), [25.0, 50.0, 100.0])
        self.assertEqual(exposure.bracket(10), [5.0, 10.0, 60.0])
        for invalid in (0, -1, math.nan, math.inf, True, "50"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                exposure.bracket(invalid)

    def test_brightest_patch_and_point_from_means(self):
        means = np.zeros((24, 3))
        means[18] = [0.10, 0.13, 0.08]
        means[3] = [0.05, 0.05, 0.05]
        self.assertEqual(exposure.brightest_patch(means), 19)
        status = [None] * 24
        point = exposure.point_from_means("x", means, status, 19, 50002.45, 1.0, 4095)
        self.assertAlmostEqual(point.peak_dn, 0.13 * 4095)
        self.assertEqual((point.peak_channel, point.patch, point.signal), ("G", 19, 50002.45))
        with self.assertRaises(ValueError):
            exposure.point_from_means("x", means, status, 25, 1.0, 1.0, 4095)


class FitTests(unittest.TestCase):
    def test_linear_fit_recovers_black_offset_and_ettr_exposure(self):
        points = [_point(ms) for ms in exposure.bracket(50)]
        fit = exposure.fit(points, gain=1.0, exposure_offset_us=OFFSET_US)
        self.assertAlmostEqual(fit.offset_dn, BLACK_DN, places=6)
        self.assertAlmostEqual(fit.slope_dn_per_us, SLOPE, places=10)
        target = 0.8 * 4095
        expected_ms = ((target - BLACK_DN) / SLOPE - OFFSET_US) / 1000
        self.assertAlmostEqual(fit.suggested_exposure_ms, round(expected_ms, 1))
        self.assertEqual((fit.points_used, fit.points_excluded), (3, 0))
        self.assertTrue(any("Extrapolated" in warning for warning in fit.warnings))
        # The suggested exposure lands on the target within the 5 % tolerance.
        predicted = fit.predicted_dn(fit.suggested_exposure_ms * 1000 + OFFSET_US, 1.0)
        self.assertLess(abs(exposure.deviation(predicted, target)), exposure.TOLERANCE)

    def test_suggestion_at_a_different_gain_scales_the_exposure(self):
        points = [_point(ms) for ms in (100, 150, 200)]
        at_one = exposure.fit(points, gain=1.0, exposure_offset_us=OFFSET_US)
        at_two = exposure.fit(points, gain=2.0, exposure_offset_us=OFFSET_US)
        self.assertAlmostEqual(at_two.suggested_exposure_ms * 1000 + OFFSET_US,
                               (at_one.suggested_exposure_ms * 1000 + OFFSET_US) / 2, delta=100)

    def test_saturated_points_are_excluded_from_the_fit(self):
        points = [_point(200), _point(300), _point(400, status="clipped")]
        fit = exposure.fit(points, gain=1.0, exposure_offset_us=OFFSET_US)
        self.assertEqual((fit.points_used, fit.points_excluded), (2, 1))
        self.assertAlmostEqual(fit.offset_dn, BLACK_DN, places=6)

    def test_not_enough_unsaturated_points(self):
        cases = (
            ([_point(400, status="clipped"), _point(500, status="clipped")], "Every exposure saturates"),
            ([_point(100), _point(400, status="clipped")], "Only one exposure"),
            ([_point(100)], "At least two"),
            ([_point(100), _point(100, label="again")], "At least two"),
        )
        for points, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                exposure.fit(points, gain=1.0, exposure_offset_us=OFFSET_US)

    def test_flat_or_inconsistent_responses_are_rejected_or_flagged(self):
        flat = [_point(ms, slope=0.0) for ms in (25, 50, 100)]
        with self.assertRaisesRegex(ValueError, "does not brighten"):
            exposure.fit(flat, gain=1.0, exposure_offset_us=OFFSET_US)
        inconsistent = [_point(25), _point(50), _point(60, status="clipped")]
        fit = exposure.fit(inconsistent, gain=1.0, exposure_offset_us=OFFSET_US)
        self.assertTrue(any("inconsistent" in warning for warning in fit.warnings))
        mixed = [exposure.ExposurePoint("a", 1.0, 1.0, 19, 100.0, "G", None),
                 exposure.ExposurePoint("b", 2.0, 1.0, 18, 200.0, "G", None)]
        with self.assertRaisesRegex(ValueError, "same chart patch"):
            exposure.fit(mixed, gain=1.0, exposure_offset_us=OFFSET_US)

    def test_frame_period_limit_caps_the_exposure_and_never_changes_the_gain(self):
        fit = exposure.fit([_point(ms) for ms in (25, 50, 100)], gain=1.0, exposure_offset_us=OFFSET_US)
        free = exposure.suggest(fit.target_signal, gain=1.0, exposure_offset_us=OFFSET_US)
        self.assertEqual((free.exposure_ms, free.gain, free.note, free.capped),
                         (fit.suggested_exposure_ms, 1.0, "", False))
        roomy = exposure.suggest(fit.target_signal, gain=1.0, exposure_offset_us=OFFSET_US, limit_ms=900.0)
        self.assertEqual(roomy, free)
        limited = exposure.suggest(fit.target_signal, gain=1.0, exposure_offset_us=OFFSET_US, limit_ms=180.0,
                                   fit=fit)
        self.assertEqual((limited.exposure_ms, limited.gain, limited.capped), (176.4, 1.0, True))
        self.assertAlmostEqual(limited.expected_dn, fit.predicted_dn(176.4 * 1000 + OFFSET_US, 1.0))
        self.assertLess(limited.expected_dn, fit.target_dn)
        self.assertIn("frame-period limit", limited.note)
        self.assertIn("Gain stays x1", limited.note)
        # A non-default configured gain is kept, not replaced.
        at_two = exposure.suggest(fit.target_signal, gain=2.0, exposure_offset_us=OFFSET_US, limit_ms=180.0)
        self.assertEqual(at_two.gain, 2.0)
        with self.assertRaisesRegex(ValueError, "no usable exposure"):
            exposure.suggest(fit.target_signal, gain=1.0, exposure_offset_us=OFFSET_US, limit_ms=0.01)

    def test_deviation_is_relative_to_the_target(self):
        self.assertAlmostEqual(exposure.deviation(3440, 3276), (3440 - 3276) / 3276)
        self.assertLess(abs(exposure.deviation(3300, 3276)), exposure.TOLERANCE)
        self.assertGreater(abs(exposure.deviation(3500, 3276)), exposure.TOLERANCE)


if __name__ == "__main__":
    unittest.main()
