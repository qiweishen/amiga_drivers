"""FX10 focus assist: edge-ESF FWHM over the whole line, magnifier window, screen images and
live-stream reading; synthetic data, no camera."""

import json
import math
import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np

from app.decoder import fx10
from app.decoder.ebus import recording
from app.decoder.fx10 import focus
from app.services import focus_stream, fx10_focus
from app.tests.test_ebus_raw_format import fixture_header, fixture_image, fixture_record

MONO12 = 0x01100005
_phi = np.vectorize(lambda x: 0.5 * (1.0 + math.erf(x / math.sqrt(2.0))))


def edge_profile(samples: int, sigma: float, edges, low=400.0, high=3000.0) -> np.ndarray:
    """Alternating rising/falling steps at ``edges``, Gaussian-blurred and sampled at pixel centres."""
    x = np.arange(samples, dtype=np.float64)
    profile = np.full(samples, low)
    for k, position in enumerate(edges):
        profile += (1 if k % 2 == 0 else -1) * (high - low) * _phi((x - position) / sigma)
    return profile


def edge_cube(sigma: float, bands=224, samples=1024, edges=None) -> np.ndarray:
    """The same line in every band, with a different gain per band (normalization must remove it)."""
    edges = np.arange(40.3, samples - 40, 40.0) if edges is None else edges
    gains = np.linspace(0.6, 1.2, bands)[:, None]
    return gains * edge_profile(samples, sigma, edges)[None, :]


def decode_png(data: bytes) -> np.ndarray:
    return cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_UNCHANGED)


def capture(samples=1024, bands=224, wavelengths=()) -> fx10.Capture:
    return fx10.Capture(samples, bands, 2, 4095, "Mono12", tuple(wavelengths), 50.0)


class MeasureTests(unittest.TestCase):
    def test_fwhm_recovers_the_blur(self):
        for sigma in (0.8, 1.2, 2.0):
            with self.subTest(sigma=sigma):
                result = focus.measure(edge_cube(sigma))
                self.assertGreaterEqual(len(result.edges), 20)
                self.assertAlmostEqual(result.fwhm_px, focus.FWHM_PER_SIGMA * sigma, delta=0.03 * sigma)
                for zone in result.zones_px:
                    self.assertAlmostEqual(zone, focus.FWHM_PER_SIGMA * sigma, delta=0.03 * sigma)

    def test_more_blur_gives_a_larger_fwhm(self):
        self.assertLess(focus.measure(edge_cube(1.0)).fwhm_px, focus.measure(edge_cube(1.6)).fwhm_px)

    def test_edges_have_subpixel_positions_and_polarity(self):
        result = focus.measure(edge_cube(1.0, edges=[200.3, 260.3, 600.7]))
        self.assertEqual([edge.rising for edge in result.edges], [True, False, True])
        np.testing.assert_allclose([edge.x for edge in result.edges], [200.3, 260.3, 600.7], atol=0.05)

    def test_a_bar_ladder_is_not_measured(self):
        cube = edge_cube(1.0, edges=np.arange(40.3, 500, 40.0))
        cube[:, 600:800] = np.where((np.arange(600, 800) // 3) % 2, 3000.0, 400.0)  # 3 px bars
        edges = focus.measure(cube).edges
        self.assertTrue(edges)
        self.assertFalse([edge.x for edge in edges if 590 <= edge.x <= 810])

    def test_flat_line_has_no_edges_and_no_value(self):
        result = focus.measure(np.full((224, 1024), 1000.0))
        self.assertEqual((result.edges, result.fwhm_px), ((), None))
        self.assertEqual(result.zones_px, (None, None, None))

    def test_band_groups_use_the_nominal_axis_without_wavelengths(self):
        result = focus.measure(edge_cube(1.0))
        self.assertFalse(result.calibrated)
        self.assertEqual(len(result.group_nm), focus.BAND_GROUPS)
        self.assertTrue(400.0 < result.group_nm[0] < result.group_nm[-1] < 1000.0)
        self.assertTrue(all(value is not None for value in result.group_fwhm_px))
        recorded = focus.measure(edge_cube(1.0, bands=4), wavelengths_nm=(500.0, 600.0, 700.0, 800.0))
        self.assertTrue(recorded.calibrated)
        self.assertEqual(recorded.group_nm, (500.0, 600.0, 700.0, 800.0))

    def test_erf_table_matches_the_standard_library(self):
        x = np.linspace(-7.0, 7.0, 1001)
        np.testing.assert_allclose(focus.erf(x), [math.erf(value) for value in x], atol=1e-7)


class LoupeWindowTests(unittest.TestCase):
    def test_window_is_centred_and_moved_inside_the_frame(self):
        self.assertEqual(focus.loupe_window(1024, 224, None), focus.Window(448, 80, 576, 144))
        self.assertEqual(focus.loupe_window(1024, 224, (0, 0)), focus.Window(0, 0, 128, 64))
        self.assertEqual(focus.loupe_window(1024, 224, (5000, 5000)), focus.Window(896, 160, 1024, 224))
        self.assertEqual(focus.loupe_window(100, 30, (50, 15)), focus.Window(0, 0, 100, 30))

    def test_non_finite_centre_is_rejected(self):
        with self.assertRaises(ValueError):
            focus.loupe_window(1024, 224, (math.nan, 10.0))


class AnalyzeTests(unittest.TestCase):
    def test_frames_are_averaged_and_shown_as_linear_dn(self):
        frames = np.stack([np.full((224, 1024), 1000, np.uint16), np.full((224, 1024), 3000, np.uint16)])
        frames[1, 0, :8] = 4095
        sample = focus.analyze(frames, capture(), 41, (10.0, 5.0))
        self.assertEqual((sample.seq, sample.frames, sample.samples, sample.bands), (41, 2, 1024, 224))
        self.assertAlmostEqual(sample.clipped_pct, 8 / frames.size * 100.0)
        image = decode_png(sample.image_png)
        self.assertEqual((image.dtype, image.shape), (np.dtype("uint8"), (224, 1024)))
        self.assertEqual(int(image[100, 100]), round(2000 / 4095 * 255))  # mean DN / full scale, no stretch
        self.assertEqual(sample.loupe, focus.Window(0, 0, 128, 64))
        np.testing.assert_array_equal(decode_png(sample.loupe_png), image[0:64, 0:128])

    def test_empty_stack_is_rejected(self):
        with self.assertRaises(ValueError):
            focus.analyze(np.empty((0, 4, 4), np.uint16), capture(4, 4), 0, None)


class StreamPlanTests(unittest.TestCase):
    def test_average_covers_about_a_fifth_of_a_second(self):
        for rate, expected in ((1.0, 1), (2.4, 1), (50.0, 10), (163.02, 16), (324.373, 16)):
            with self.subTest(rate=rate):
                self.assertEqual(fx10_focus.average_frames(rate), expected)


class LiveStreamTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.camera = Path(self.temp.name) / "fx10_stream"
        self.camera.mkdir()
        self.bands, self.samples = 8, 64
        (self.camera / "device.json").write_text(json.dumps({
            "format": "ebus-device-eBUS_SDK_v6.5.1.6797", "kind": "fx10", "id": "fx10",
            "recording_format": "ebus-raw-eBUS_SDK_v6.5.1.6797",
            "sdk": {"name": "Pleora eBUS", "header_version": "6.5.1.6797"},
            "profile": {"samples": self.samples, "bands": self.bands, "pixel_format": "Mono12", "full_scale": 4095,
                        "expected_frame_rate_hz": 50.0, "wavelengths_nm": []}}), encoding="utf-8")

    def record(self, sequence: int, value: int) -> bytes:
        image = np.full((self.bands, self.samples), value, "<u2")
        payload = image.tobytes()
        meta = fixture_image(len(payload))
        meta["image"].update(pixel_format=MONO12, width=self.samples, height=self.bands, padding_x=0,
                             padding_y=0, image_size=len(payload), effective_image_size=len(payload),
                             payload_offset=0)
        return fixture_record(meta, payload, sequence)

    def segment(self, index: int, body: bytes = b"") -> Path:
        path = self.camera / f"seg_{index:05d}.raw"
        path.write_bytes(fixture_header(index) + body)
        return path

    def test_reading_averages_the_newest_frames_and_prunes_closed_segments(self):
        for index in range(1, 6):  # two frames per segment; frame n has 100 * (n + 1) DN
            first = 2 * (index - 1)
            self.segment(index, self.record(first, 100 * (first + 1)) + self.record(first + 1, 100 * (first + 2)))
        self.segment(6)  # just created by the writer: header only
        cap = fx10.load_capture(self.camera)
        sample = fx10_focus._read_newest(self.camera, cap, None, 3, None)
        self.assertEqual((sample.seq, sample.frames), (9, 3))
        image = decode_png(sample.image_png)
        self.assertEqual(int(image[0, 0]), round(900 / 4095 * 255))  # mean of 800, 900, 1000 DN
        names = [path.name for path in recording.segment_paths(self.camera)]
        self.assertEqual(names, ["seg_00004.raw", "seg_00005.raw", "seg_00006.raw"])
        self.assertIsNone(fx10_focus._read_newest(self.camera, cap, 9, 3, None))

    def test_camera_directory_waits_for_the_first_segment(self):
        output = self.camera.parent
        self.assertIsNone(focus_stream.camera_directory(output))
        self.segment(1)
        self.assertEqual(focus_stream.camera_directory(output), self.camera)


if __name__ == "__main__":
    unittest.main()
