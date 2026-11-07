"""GOX display encoding contracts on synthetic linear images; no camera, no detector."""

import unittest

import cv2
import numpy as np

from app.decoder.gox import render


def _decode(png: bytes, depth: int) -> np.ndarray:
    if png[:8] != b"\x89PNG\r\n\x1a\n" or png[24] != depth:
        raise AssertionError(f"Expected a {depth}-bit PNG")
    decoded = cv2.imdecode(np.frombuffer(png, np.uint8), cv2.IMREAD_UNCHANGED)
    return decoded[..., ::-1].astype(np.float64) / (65535.0 if depth == 16 else 255.0)


def _gray(levels) -> np.ndarray:
    values = np.asarray(levels, dtype=np.float32).reshape(1, -1, 1)
    return np.repeat(values, 3, axis=2).copy()


class GoxRenderTests(unittest.TestCase):
    def test_no_exposure_adjustment_only_clip_and_srgb(self):
        linear = _gray([-0.2, 0.0, 0.05, 0.18, 1.0, 3.0])
        before = linear.copy()
        values = _decode(render.full_png16(linear), 16)[0, :, 1]
        expected = render.srgb_oetf(np.array([0.0, 0.0, 0.05, 0.18, 1.0, 1.0]))
        np.testing.assert_allclose(values, expected, atol=1 / 65535)
        np.testing.assert_array_equal(linear, before)  # the input stays intact

    def test_scale_is_fixed_not_per_image(self):
        dark, bright = _gray([0.1] * 4), _gray([0.1, 0.1, 5.0, 5.0])
        self.assertAlmostEqual(_decode(render.full_png16(dark), 16)[0, 0, 0],
                               _decode(render.full_png16(bright), 16)[0, 0, 0], places=6)

    def test_screen_image_is_downscaled_8_bit_with_the_same_encoding(self):
        linear = np.full((40, 3200, 3), 0.18, dtype=np.float32)
        screen = _decode(render.display_png(linear), 8)
        self.assertEqual(max(screen.shape[:2]), render.DISPLAY_LONG_EDGE)
        np.testing.assert_allclose(screen, render.srgb_oetf(0.18), atol=1 / 255)

    def test_rejects_non_float32_or_nonfinite_input(self):
        for linear in (np.zeros((2, 2, 3), dtype=np.float64),
                       np.full((2, 2, 3), np.nan, dtype=np.float32),
                       np.zeros((2, 2), dtype=np.float32)):
            with self.subTest(dtype=linear.dtype, shape=linear.shape), self.assertRaises(ValueError):
                render.full_png16(linear)


if __name__ == "__main__":
    unittest.main()
