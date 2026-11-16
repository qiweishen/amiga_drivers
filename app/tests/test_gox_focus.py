"""GoX focus assist: BayerRG8 Gr-plane sharpness, ROI geometry, stream rate and live-stream reading; no camera."""

import math
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import cv2
import numpy as np

from app.decoder.ebus import pixels, recording
from app.decoder.gox import focus
from app.services import gox_focus, gox_timing
from app.tests.test_ebus_raw_format import fixture_header, fixture_image, fixture_record

BAYER_RG8 = 0x01080009


def native_metadata(image: np.ndarray, *, padding_x=0, offset=0, padding_y=0, pf=BAYER_RG8):
    """BayerRG8 payload: one byte per photosite, with optional leading bytes and row/trailing padding."""
    height, width = image.shape
    payload = (b"\xaa" * offset + b"".join(image[y].astype(np.uint8).tobytes() + b"\xee" * padding_x
                                          for y in range(height)) + b"\xcc" * padding_y)
    meta = fixture_image(len(payload))
    meta["image"].update(pixel_format=pf, width=width, height=height, padding_x=padding_x, padding_y=padding_y,
                         image_size=(width + padding_x) * height + padding_y,
                         effective_image_size=width * height, payload_offset=offset)
    return meta, payload


def native_record(image: np.ndarray, *, seq=0, **layout):
    meta, payload = native_metadata(image, **layout)
    return recording.Record(Path("unused"), 0, seq, meta, 1, len(payload), 0), payload


def cfa_image(width, height, r=60, gr=120, gb=180, b=30) -> np.ndarray:
    image = np.empty((height, width), np.uint8)
    image[0::2, 0::2], image[0::2, 1::2] = r, gr
    image[1::2, 0::2], image[1::2, 1::2] = gb, b
    return image


def decode_png(data: bytes) -> np.ndarray:
    return cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_UNCHANGED)


class ImageRowsTests(unittest.TestCase):
    def test_rows_view_matches_the_full_decode_with_padding_and_offset(self):
        image = np.random.default_rng(7).integers(0, 256, (6, 8), dtype=np.uint8)
        record, payload = native_record(image, padding_x=3, offset=5, padding_y=2)
        decoded, name, _ = pixels.decode_record(record, payload)
        self.assertEqual(name, "BayerRG8")
        np.testing.assert_array_equal(decoded, image)
        rows = pixels.image_rows(record, payload)
        self.assertEqual((rows.dtype, rows.shape), (np.dtype("uint8"), (6, 8)))
        np.testing.assert_array_equal(rows, image)


class RoiWindowTests(unittest.TestCase):
    def test_window_is_tile_aligned_and_moved_inside_the_frame(self):
        self.assertEqual(focus.roi_window(4128, 3008, None, 200), focus.Roi(1864, 1304, 2264, 1704))
        self.assertEqual(focus.roi_window(4128, 3008, (0, 0), 200), focus.Roi(0, 0, 400, 400))
        self.assertEqual(focus.roi_window(4128, 3008, (5000, 5000), 200), focus.Roi(3728, 2608, 4128, 3008))
        self.assertEqual(focus.roi_window(4128, 3008, (101.0, 77.0), 32), focus.Roi(68, 44, 132, 108))
        self.assertEqual(focus.roi_window(100, 60, (50, 30), 5000), focus.Roi(0, 0, 100, 60))
        smallest = focus.roi_window(4128, 3008, None, 1)
        self.assertEqual(smallest.x1 - smallest.x0, 2 * focus.MIN_HALF_PX)

    def test_non_finite_centre_is_rejected(self):
        with self.assertRaises(ValueError):
            focus.roi_window(4128, 3008, (math.nan, 10.0), 200)


class SharpnessTests(unittest.TestCase):
    def setUp(self):
        self.pattern = np.zeros((64, 64), np.float32)
        self.pattern[:, 0::4] = self.pattern[:, 1::4] = 1.0  # 2-photosite stripes

    def test_pedestal_is_the_documented_8_bit_black_level(self):
        self.assertEqual(focus.PEDESTAL_DN, 8.0)
        self.assertEqual(focus.FULL_SCALE_DN, 255)

    def test_score_ignores_illumination_above_the_pedestal(self):
        dim, dim_signal = focus.sharpness(focus.PEDESTAL_DN + 100.0 * self.pattern)
        bright, bright_signal = focus.sharpness(focus.PEDESTAL_DN + 200.0 * self.pattern)
        self.assertAlmostEqual(dim_signal, 50.0, places=4)
        self.assertAlmostEqual(bright_signal, 100.0, places=4)
        self.assertGreater(dim, 0.0)
        self.assertAlmostEqual(bright / dim, 1.0, places=5)

    def test_blur_lowers_the_score(self):
        sharp = focus.PEDESTAL_DN + 200.0 * self.pattern
        blurred = cv2.GaussianBlur(sharp, (0, 0), 1.5)
        self.assertLess(focus.sharpness(blurred)[0], 0.5 * focus.sharpness(sharp)[0])

    def test_dark_roi_is_not_scored(self):
        score, signal = focus.sharpness(np.full((32, 32), 10, np.uint8))
        self.assertIsNone(score)
        self.assertAlmostEqual(signal, 2.0)


class AnalyzeTests(unittest.TestCase):
    def test_only_gr_photosites_are_scored_and_magnified(self):
        record, payload = native_record(cfa_image(128, 96), seq=12)
        sample = focus.analyze(record, payload, (51.7, 40.2), 32)
        self.assertEqual(sample.roi, focus.Roi(18, 8, 82, 72))
        self.assertEqual(sample.seq, 12)
        self.assertAlmostEqual(sample.signal_dn, 120 - focus.PEDESTAL_DN)
        self.assertEqual(sample.score, 0.0)  # uniform Gr plane
        self.assertEqual(sample.clipped_pct, 0.0)
        self.assertEqual(sample.loupe_origin, (18, 8))
        loupe = decode_png(sample.loupe_png)
        self.assertEqual((loupe.dtype, loupe.shape), (np.dtype("uint8"), (32, 32)))
        self.assertTrue(np.all(loupe == 120))  # the photosite values themselves
        self.assertIsNone(sample.overview_png)

    def test_clipping_counts_every_cfa_colour(self):
        record, payload = native_record(cfa_image(64, 64, r=255))
        self.assertEqual(focus.analyze(record, payload, None, 32).clipped_pct, 25.0)

    def test_large_roi_magnifies_its_centre(self):
        record, payload = native_record(cfa_image(1200, 800))
        sample = focus.analyze(record, payload, None, 400)
        self.assertEqual(sample.roi, focus.Roi(200, 0, 1000, 800))
        self.assertEqual(decode_png(sample.loupe_png).shape, (focus.LOUPE_MAX_PX, focus.LOUPE_MAX_PX))
        self.assertEqual(sample.loupe_origin, (200 + 2 * 72, 0 + 2 * 72))

    def test_overview_scale_maps_back_to_full_resolution(self):
        record, payload = native_record(cfa_image(32, 16))
        with patch.object(focus, "OVERVIEW_LONG_EDGE", 8):
            sample = focus.analyze(record, payload, None, 32, overview=True)
        overview = decode_png(sample.overview_png)
        self.assertEqual(overview.shape, (4, 8))
        self.assertTrue(np.all(overview == 120))
        self.assertEqual(sample.overview_scale, (4.0, 4.0))

    def test_other_pixel_formats_are_rejected_before_decoding(self):
        meta, payload = native_metadata(cfa_image(8, 4))
        for code in (0x01080001, 0x010C002B):  # Mono8; the recording format BayerRG12Packed
            meta["image"]["pixel_format"] = code
            record = recording.Record(Path("unused"), 0, 0, meta, 1, len(payload), 0)
            with self.subTest(code=code), self.assertRaisesRegex(ValueError, "BayerRG8"):
                focus.analyze(record, payload, None, 32)


class StreamRateTests(unittest.TestCase):
    def test_focus_rate_is_the_maximum_under_the_8_bit_ceiling(self):
        self.assertEqual(gox_timing.MAX_FOCUS_FRAME_RATE_HZ, 9.079)
        for exposure_ms, expected in ((10.0, 9.079), (99.12, 9.079), (99.13, 9.078), (100.0, 9.0),
                                      (150.0, 6.0), (399.405, 2.253)):
            with self.subTest(exposure_ms=exposure_ms):
                rate = gox_timing.max_frame_rate_hz(exposure_ms, gox_timing.MAX_FOCUS_FRAME_RATE_HZ)
                self.assertEqual(rate, expected)
                self.assertTrue(gox_timing.exposure_fits(exposure_ms, rate))
                self.assertEqual(gox_focus.request_for("10.95.1.101", exposure_ms, 1.0).frame_rate_hz, expected)
        self.assertEqual(gox_timing.max_frame_rate_hz(10.0), gox_timing.MAX_FRAME_RATE_HZ)  # default unchanged

    def test_request_rejects_an_exposure_outside_the_camera_range(self):
        for exposure_ms in (0.0, 399.406, math.nan):
            with self.subTest(exposure_ms=exposure_ms), self.assertRaises(ValueError):
                gox_focus.request_for("10.95.1.101", exposure_ms, 1.0)


class LiveStreamTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.camera = Path(self.temp.name) / "cam0"
        self.camera.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def segment(self, index: int, body: bytes = b"", *, header=True) -> Path:
        path = self.camera / f"seg_{index:05d}.raw"
        path.write_bytes((fixture_header(index) if header else b"") + body)
        return path

    def test_newest_valid_record_skips_a_segment_still_being_created(self):
        meta, payload = native_metadata(cfa_image(8, 4))
        self.segment(1, fixture_record(meta, payload, 0))
        self.segment(2, fixture_record(meta, payload, 1))
        newest = self.segment(3, header=False)
        self.assertEqual(recording.newest_valid_record(self.camera).sequence, 1)
        newest.write_bytes(fixture_header(3)[:30])  # header still being written
        self.assertEqual(recording.newest_valid_record(self.camera).sequence, 1)
        self.segment(3)  # header only
        self.assertEqual(recording.newest_valid_record(self.camera).sequence, 1)
        self.segment(3, fixture_record(meta, payload, 2)[:40])  # torn record
        self.assertEqual(recording.newest_valid_record(self.camera).sequence, 1)
        self.segment(3, fixture_record(dict(meta, valid_frame=False), payload, 2))  # flagged buffer
        self.assertEqual(recording.newest_valid_record(self.camera).sequence, 1)
        self.segment(3, fixture_record(meta, payload, 2))
        self.assertEqual(recording.newest_valid_record(self.camera).sequence, 2)

    def test_reading_prunes_closed_segments_and_skips_a_seen_frame(self):
        meta, payload = native_metadata(cfa_image(64, 64))
        for index in range(1, 6):
            self.segment(index, fixture_record(meta, payload, index - 1))
        sample = gox_focus._read_newest(self.camera, None, None, 32, False)
        self.assertEqual(sample.seq, 4)
        names = [path.name for path in recording.segment_paths(self.camera)]
        self.assertEqual(names, ["seg_00003.raw", "seg_00004.raw", "seg_00005.raw"])
        self.assertIsNone(gox_focus._read_newest(self.camera, 4, None, 32, False))


if __name__ == "__main__":
    unittest.main()
