"""Static route display contracts; these tests do not connect to a receiver."""

import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

from app.decoder.asterx import track
from app.decoder.asterx.recording import CSV_COLUMNS


def csv_row(stamp, lat, lon):
    values = {name: "0" for name in CSV_COLUMNS["live_insnavgeod.csv"]}
    values.update(host_unix_ns=str(stamp), lat_deg=str(lat), lon_deg=str(lon))
    return ",".join(values[name] for name in CSV_COLUMNS["live_insnavgeod.csv"]) + "\n"


class AsterxTrackTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.session = Path(temporary.name)
        self.source = self.session / "raw" / "asterx" / "live_insnavgeod.csv"
        self.source.parent.mkdir(parents=True)

    def write(self, rows, tail=""):
        header = ",".join(CSV_COLUMNS[self.source.name]) + "\n"
        self.source.write_text(header + "".join(rows) + tail, encoding="utf-8")

    def read(self):
        return track.read_track(self.session, threading.Event())

    def test_simplification_retains_full_extent_and_original_points(self):
        points = [(0.0, float(index)) for index in range(30)]
        points[3] = (-40.0, 3.0)
        points[4] = (40.0, 4.0)
        self.write([csv_row(1_000_000_000 + index * 10_000_000, *point)
                    for index, point in enumerate(points)])
        before = self.source.read_bytes()
        with patch.object(track, "MAX_TRACK_POINTS", 6):
            result = self.read()
        self.assertTrue(result.simplified)
        self.assertLessEqual(result.displayed_points, 6)
        self.assertEqual(result.valid_points, 30)
        self.assertEqual(result.bounds, ((-40.0, 0.0), (40.0, 29.0)))
        self.assertEqual(result.segments[0][0], points[0])
        self.assertEqual(result.segments[0][-1], points[-1])
        self.assertTrue(all(point in points for segment in result.segments for point in segment))
        self.assertEqual(self.source.read_bytes(), before)

    def test_invalid_rows_gaps_and_clock_reversal_split_the_route(self):
        self.write([csv_row(1_000_000_000, 1, 1), csv_row(2_000_000_000, 2, 2),
                    "malformed,row\n", csv_row(3_000_000_000, 3, 3),
                    csv_row(6_000_000_000, 4, 4), csv_row(5_000_000_000, 5, 5),
                    csv_row(6_000_000_000, "nan", 6), csv_row(7_000_000_000, 95, 7),
                    csv_row(8_000_000_000, 8, 8)], tail="incomplete,row")
        result = self.read()
        self.assertEqual(result.segments, (((1.0, 1.0), (2.0, 2.0)), ((3.0, 3.0),),
                                           ((4.0, 4.0),), ((5.0, 5.0),), ((8.0, 8.0),)))
        self.assertEqual(result.skipped_rows, 3)
        self.assertEqual(result.partial_rows, 1)
        self.assertEqual(result.breaks, 4)
        self.assertEqual(result.bounds, ((1.0, 1.0), (8.0, 8.0)))

    def test_limits_fail_instead_of_publishing_a_partial_range(self):
        self.write([csv_row(1, 1, 1), csv_row(2, 2, 2)])
        for name, limit in (("MAX_TRACK_BYTES", 1), ("MAX_TRACK_ROWS", 1)):
            with self.subTest(limit=name), patch.object(track, name, limit):
                with self.assertRaisesRegex(ValueError, "no partial range"):
                    self.read()
        self.write([csv_row(1, 1, 1), "bad,row\n", csv_row(2, 2, 2)])
        with patch.object(track, "MAX_TRACK_SEGMENTS", 1):
            with self.assertRaisesRegex(ValueError, "no partial range"):
                self.read()

    def test_cancellation_and_missing_valid_coordinates(self):
        cancel = threading.Event()
        cancel.set()
        with self.assertRaises(track.TrackCancelled):
            track.read_track(self.session, cancel)
        self.write([csv_row(0, 1, 1), csv_row(1, 1, "nan")])
        with self.assertRaisesRegex(ValueError, "No valid timestamped coordinates"):
            self.read()


if __name__ == "__main__":
    unittest.main()
