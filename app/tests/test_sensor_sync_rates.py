"""SensorSync pair-rate rule (log format 3); the same boundaries as the C++ copy, no board."""

import math
import unittest

from app.services import sensor_sync_rates as rates


class SensorSyncRatesTests(unittest.TestCase):
    def test_zero_turns_a_pair_off_and_bad_numbers_are_refused(self):
        self.assertTrue(rates.rate_accepted(rates.FX, 0.0))
        self.assertTrue(rates.rate_accepted(rates.JAI, 0.0))
        for hz in (-1.0, math.nan, math.inf, 1e-300):
            with self.subTest(hz=hz):
                self.assertFalse(rates.rate_accepted(rates.JAI, hz))

    def test_boundaries_match_common_sensor_sync_rates_h(self):
        cases = ((rates.FX, 17.881, False), (rates.FX, 17.882, True), (rates.FX, 50.0, True),
                 (rates.JAI, 0.1396, False), (rates.JAI, 0.1398, True), (rates.JAI, 6.054, True),
                 (rates.JAI, 25.0, True), (rates.FX, 9847.689, True), (rates.FX, 9847.69, False),
                 (rates.JAI, 9847.69, False))
        for group, hz, accepted in cases:
            with self.subTest(group=group, hz=hz):
                self.assertEqual(rates.rate_accepted(group, hz), accepted)

    def test_range_text_names_accepted_bounds(self):
        self.assertEqual(rates.range_text(rates.FX), "[17.882, 9847.689] Hz")
        self.assertEqual(rates.range_text(rates.JAI), "[0.140, 9847.689] Hz")
        self.assertEqual(rates.group_of(1), rates.FX)
        self.assertEqual(rates.group_of(2), rates.JAI)


if __name__ == "__main__":
    unittest.main()
