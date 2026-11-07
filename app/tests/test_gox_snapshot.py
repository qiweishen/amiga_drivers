"""Native GOX DN and 16-bit PNG contracts; no device or capture is started."""

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import cv2
import numpy as np

from app.decoder.gox import preview, recording
from app.tests.test_gox_format import write_gox_fixture


def _frame(raw, pixel_format="BayerRG12Packed"):
    return SimpleNamespace(image=raw, pixel_format=pixel_format)


def _png_rgb(encoded_bytes):
    # PNG signature and IHDR bit-depth byte independently document the container.
    if encoded_bytes[:8] != b"\x89PNG\r\n\x1a\n" or encoded_bytes[24] != 16:
        raise AssertionError("Preview is not a 16-bit PNG")
    decoded = cv2.imdecode(np.frombuffer(encoded_bytes, np.uint8), cv2.IMREAD_UNCHANGED)
    return cv2.cvtColor(decoded, cv2.COLOR_BGR2RGB)


class SnapshotNativeDnTests(unittest.TestCase):
    def test_native_preview_png_retains_every_12_bit_code(self):
        dn = np.arange(4096, dtype=np.uint16).reshape(64, 64)
        rgb = np.repeat(dn[:, :, None], 3, axis=2)
        original = rgb.copy()
        decoded = _png_rgb(preview.native_full_png16(rgb))
        self.assertEqual(decoded.dtype, np.dtype("uint16"))
        self.assertEqual(decoded.shape, rgb.shape)
        np.testing.assert_array_equal(decoded, rgb << 4)
        np.testing.assert_array_equal(decoded >> 4, original)
        np.testing.assert_array_equal(rgb, original)

    def test_encoder_preserves_full_16_bit_corrected_rgb_codes(self):
        rgb = np.array([[[1, 1001, 2002], [3003, 65534, 65535]]], dtype=np.uint16)
        np.testing.assert_array_equal(_png_rgb(preview.png16_bytes(rgb)), rgb)

    def test_demosaic_receives_native_dn_and_keeps_rgb_channel_order(self):
        raw = np.full((8, 8), 1001, dtype=np.uint16)
        original = raw.copy()
        bgr = np.broadcast_to(np.array([103, 205, 307], dtype=np.uint16), (8, 8, 3)).copy()
        with patch.object(preview.pixels, "demosaic", return_value=bgr) as demosaic:
            rgb = preview.native_rgb(_frame(raw))
        supplied, pattern = demosaic.call_args.args
        self.assertIs(supplied, raw)
        self.assertEqual(pattern, "RG")
        self.assertEqual(rgb.dtype, np.dtype("uint16"))
        np.testing.assert_array_equal(rgb[3, 3], [307, 205, 103])
        np.testing.assert_array_equal(raw, original)

    def test_snapshot_metrics_use_original_photosites_and_native_dn_bins(self):
        raw = np.zeros((8, 8), dtype=np.uint16)
        raw[::2, ::2] = 4095
        frame = _frame(raw)
        original = raw.copy()
        with patch.object(preview, "snapshot_frame", return_value=frame):
            result = preview.snapshot(Path("unused"))
        self.assertEqual(result.clipped_pct, 25.0)
        self.assertEqual(result.mean_dn, 1023.75)
        self.assertEqual(result.histogram, [48] + [0] * 62 + [16])
        self.assertEqual(result.display_png[24], 8)
        screen = cv2.imdecode(np.frombuffer(result.display_png, np.uint8), cv2.IMREAD_UNCHANGED)
        self.assertEqual(screen.dtype, np.dtype("uint8"))
        self.assertEqual(screen.shape, (8, 8, 3))
        self.assertEqual(preview.live_display_png(frame), result.display_png)
        with patch.object(preview, "snapshot_frame", return_value=frame):
            full = preview.snapshot_full_png16(Path("unused"))
        self.assertEqual(_png_rgb(full).shape, (8, 8, 3))
        np.testing.assert_array_equal(raw, original)

    def test_screen_preview_is_area_downscaled_linear_dn(self):
        rgb = np.zeros((10, 4000, 3), dtype=np.uint16)
        rgb[:, :2000] = 1001
        rgb[:, 2000:] = 4095
        small = preview.native_display_rgb8(rgb)
        self.assertEqual(small.shape, (4, 1600, 3))
        np.testing.assert_array_equal(small[0, 0], [round(1001 / 4095 * 255)] * 3)
        np.testing.assert_array_equal(small[0, -1], [255] * 3)
        self.assertEqual(rgb.max(), 4095)  # input untouched
        same = np.full((4, 6, 3), 2048, dtype=np.uint16)
        self.assertEqual(preview.native_display_rgb8(same).shape, (4, 6, 3))

    def test_display_points_follow_the_resize_pixel_center_convention(self):
        points = np.array([[[-0.5, -0.5], [3999.5, 9.5]]])
        mapped = preview.display_points(points, (10, 4000, 3), (4, 1600, 3))
        np.testing.assert_allclose(mapped, [[[-0.5, -0.5], [1599.5, 3.5]]])

    def test_live_full_resolution_download_re_decodes_the_same_committed_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            camera = Path(temporary) / "camera"
            write_gox_fixture(camera)
            live = recording.latest_frame(camera)
            again = recording.frame_from_record(live.record)
            np.testing.assert_array_equal(again.image, live.image)
            decoded = _png_rgb(preview.record_full_png16(live.record))
            np.testing.assert_array_equal(decoded, preview.native_rgb(live) << 4)
            self.assertEqual(preview.live_display_png(live)[24], 8)

    def test_other_pixel_formats_and_invalid_native_samples_are_rejected(self):
        cases = (
            (np.zeros((8, 8), dtype=np.uint16), "BayerRG12"),
            (np.zeros((8, 8), dtype=np.uint16), "BayerRG12p"),
            (np.zeros((8, 8), dtype=np.uint16), "Mono12"),
            (np.zeros((8, 8), dtype=np.uint8), "BayerRG12Packed"),
            (np.full((8, 8), 4096, dtype=np.uint16), "BayerRG12Packed"),
            (np.zeros((8, 8, 3), dtype=np.uint16), "BayerRG12Packed"),
        )
        for raw, name in cases:
            with self.subTest(pixel_format=name, shape=raw.shape, dtype=raw.dtype):
                with self.assertRaises(ValueError):
                    preview.live_display_png(_frame(raw, name))


if __name__ == "__main__":
    unittest.main()
