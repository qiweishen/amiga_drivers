"""GOX frame-rate / exposure limits shared by the workbench and config writeback; no camera."""

import unittest

from app.services import gox_timing


class GoxTimingTests(unittest.TestCase):
    def test_limits_mirror_the_driver_constants(self):
        self.assertEqual(gox_timing.MAX_FRAME_RATE_HZ, 6.054)
        self.assertEqual(gox_timing.MAX_EXPOSURE_MS, 399.405)
        self.assertAlmostEqual(gox_timing.exposure_limit_ms(5.0), 180.0)

    def test_max_frame_rate_uses_ninety_percent_of_the_period_and_the_camera_ceiling(self):
        for exposure_ms, expected in ((176.4, 5.102), (150.0, 6.0), (180.0, 5.0), (399.405, 2.253),
                                      (10.0, 6.054), (148.66, 6.054), (148.7, 6.052)):
            with self.subTest(exposure_ms=exposure_ms):
                rate = gox_timing.max_frame_rate_hz(exposure_ms)
                self.assertEqual(rate, expected)
                self.assertTrue(gox_timing.exposure_fits(exposure_ms, rate))
                if rate < gox_timing.MAX_FRAME_RATE_HZ:
                    self.assertFalse(gox_timing.exposure_fits(exposure_ms, rate + 0.001))

    def test_every_millisecond_step_yields_a_fitting_rate(self):
        for step in range(1, 399406):
            exposure_ms = step / 1000.0
            rate = gox_timing.max_frame_rate_hz(exposure_ms)
            if not gox_timing.exposure_fits(exposure_ms, rate) or rate > gox_timing.MAX_FRAME_RATE_HZ:
                self.fail(f"{exposure_ms} ms -> {rate} Hz does not fit")

    def test_invalid_inputs_are_rejected(self):
        for value in (0.0, -1.0, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gox_timing.max_frame_rate_hz(value)
        for rate in (0.1, 6.055, float("nan")):
            with self.subTest(rate=rate), self.assertRaises(ValueError):
                gox_timing.check_frame_rate(rate)
        self.assertEqual(gox_timing.check_frame_rate(6.054), 6.054)


if __name__ == "__main__":
    unittest.main()
