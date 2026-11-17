"""GOX and FX10 frame-rate / exposure limits shared by the workbench and config writeback; no camera."""

import unittest

from app.services import timing


class GoxTimingTests(unittest.TestCase):
    def test_limits_mirror_the_driver_constants(self):
        self.assertEqual(timing.GOX_MAX_FRAME_RATE_HZ, 6.054)
        self.assertEqual(timing.GOX_MAX_EXPOSURE_MS, 399.405)
        self.assertAlmostEqual(timing.exposure_limit_ms(5.0), 180.0)

    def test_max_frame_rate_uses_ninety_percent_of_the_period_and_the_camera_ceiling(self):
        for exposure_ms, expected in ((176.4, 5.102), (150.0, 6.0), (180.0, 5.0), (399.405, 2.253),
                                      (10.0, 6.054), (148.66, 6.054), (148.7, 6.052)):
            with self.subTest(exposure_ms=exposure_ms):
                rate = timing.gox_max_frame_rate_hz(exposure_ms)
                self.assertEqual(rate, expected)
                self.assertTrue(timing.exposure_fits(exposure_ms, rate))
                if rate < timing.GOX_MAX_FRAME_RATE_HZ:
                    self.assertFalse(timing.exposure_fits(exposure_ms, rate + 0.001))

    def test_every_millisecond_step_yields_a_fitting_rate(self):
        for step in range(1, 399406):
            exposure_ms = step / 1000.0
            rate = timing.gox_max_frame_rate_hz(exposure_ms)
            if not timing.exposure_fits(exposure_ms, rate) or rate > timing.GOX_MAX_FRAME_RATE_HZ:
                self.fail(f"{exposure_ms} ms -> {rate} Hz does not fit")

    def test_invalid_inputs_are_rejected(self):
        for value in (0.0, -1.0, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                timing.gox_max_frame_rate_hz(value)
        for rate in (0.1, 6.055, float("nan")):
            with self.subTest(rate=rate), self.assertRaises(ValueError):
                timing.gox_check_frame_rate(rate)
        self.assertEqual(timing.gox_check_frame_rate(6.054), 6.054)


class Fx10TimingTests(unittest.TestCase):
    def test_limits_mirror_the_driver_constants(self):
        self.assertEqual(timing.FX10_MAX_FRAME_RATE_HZ_UNBINNED, 163.02)
        self.assertEqual(timing.FX10_MAX_FRAME_RATE_HZ_BINNED, 324.373)
        self.assertEqual(timing.FX10_MAX_EXPOSURE_MS, 419.0)
        self.assertEqual(timing.fx10_rate_ceiling(1, 1), 163.02)
        for spatial, spectral in ((1, 2), (2, 1), (2, 2), (4, 8), (8, 8)):
            with self.subTest(spatial=spatial, spectral=spectral):
                self.assertEqual(timing.fx10_rate_ceiling(spatial, spectral), 324.373)

    def test_max_frame_rate_uses_ninety_percent_of_the_period_and_the_binning_ceiling(self):
        for exposure_ms, binning, expected in ((10.0, (1, 2), 90.0), (18.0, (1, 2), 50.0),
                                               (419.0, (1, 2), 2.147), (1.0, (1, 1), 163.02),
                                               (1.0, (1, 2), 324.373), (5.52, (1, 1), 163.02),
                                               (5.6, (1, 1), 160.714)):
            with self.subTest(exposure_ms=exposure_ms, binning=binning):
                rate = timing.fx10_max_frame_rate_hz(exposure_ms, *binning)
                self.assertEqual(rate, expected)
                self.assertTrue(timing.exposure_fits(exposure_ms, rate))
                if rate < timing.fx10_rate_ceiling(*binning):
                    self.assertFalse(timing.exposure_fits(exposure_ms, rate + 0.001))

    def test_every_millisecond_step_yields_a_fitting_rate(self):
        for binning in ((1, 1), (2, 2)):
            for step in range(10, 419001):
                exposure_ms = step / 1000.0
                rate = timing.fx10_max_frame_rate_hz(exposure_ms, *binning)
                if (not timing.exposure_fits(exposure_ms, rate)
                        or rate > timing.fx10_rate_ceiling(*binning)):
                    self.fail(f"{exposure_ms} ms at {binning} -> {rate} Hz does not fit")

    def test_invalid_inputs_are_rejected(self):
        for value in (0.0, -1.0, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                timing.fx10_max_frame_rate_hz(value, 1, 2)
        for exposure in (0.0, 419.001, float("nan")):
            with self.subTest(exposure=exposure), self.assertRaises(ValueError):
                timing.fx10_check_exposure(exposure)
        self.assertEqual(timing.fx10_check_exposure(419.0), 419.0)
        for rate, binning in ((163.03, (1, 1)), (324.374, (1, 2)), (0.0, (1, 2)), (float("nan"), (2, 2))):
            with self.subTest(rate=rate, binning=binning), self.assertRaises(ValueError):
                timing.fx10_check_frame_rate(rate, *binning)
        self.assertEqual(timing.fx10_check_frame_rate(163.02, 1, 1), 163.02)
        self.assertEqual(timing.fx10_check_frame_rate(324.373, 2, 2), 324.373)


if __name__ == "__main__":
    unittest.main()
