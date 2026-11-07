"""GOX exposure-series orchestration with substituted captures and chart analysis; no camera."""

import json
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch

import numpy as np

from app.decoder.gox import exposure
from app.services import gox_exposure, gox_reference, gox_tools

OFFSET_US = 2.45
BLACK, SLOPE = 145.0, 0.0081  # DN, DN/µs at gain x1
DEVICE = "000362"


def _means(peak_dn: float) -> np.ndarray:
    means = np.full((24, 3), 0.05)
    means[18] = [peak_dn * 0.8 / 4095, peak_dn / 4095, peak_dn * 0.6 / 4095]  # patch 19, G brightest
    return means


def _peak(exposure_ms: float, gain: float = 1.0) -> float:
    return BLACK + SLOPE * (exposure_ms * 1000 + OFFSET_US) * gain


class ExposureSeriesTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "tmp"
        self.captured = []
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(gox_exposure, "_series", None))
        self.stack.enter_context(patch.object(gox_reference, "reference_tmp_root", return_value=self.root))
        self.stack.enter_context(patch.object(gox_reference, "_config_bytes", return_value=b"cameras: []\n"))
        self.stack.enter_context(patch.object(gox_reference, "capability_reason", return_value=None))
        self.stack.enter_context(patch.object(gox_exposure.camera_operations, "run", side_effect=self.reserve))
        self.stack.enter_context(patch.object(gox_tools, "capture_raw", side_effect=self.capture_raw))
        self.analyze = self.stack.enter_context(patch.object(gox_exposure, "_analyze", side_effect=self.fake_analyze))
        self.limit = self.stack.enter_context(patch.object(gox_exposure, "_exposure_limit",
                                                           return_value=(None, [])))
        self.undetectable = set()
        self.config_gain = 1.0

    async def reserve(self, sensor, operation, *, kind):
        self.assertEqual((sensor, kind), ("gox", "gox-exposure-series"))
        return await operation()

    async def capture_raw(self, request, output, *, config, exposure_ms=None, gain=None):
        self.assertIsNone(request)
        self.assertIsNone(gain)  # the series never overrides the configured gain
        self.assertEqual(config.read_bytes(), b"cameras: []\n")
        camera = output / "cam0"
        camera.mkdir(parents=True)
        (camera / "device.json").write_text("{}", encoding="utf-8")
        self.captured.append(exposure_ms)
        return gox_tools.RawCapture(True, camera_dir=camera, raw_output=f"captured {exposure_ms} ms")

    def fake_analyze(self, camera_dir, requested_ms, chart_key, polygons=None):
        result = gox_exposure._Analysis(camera_dir, requested_ms)
        if requested_ms in self.undetectable and polygons is None:
            result.error = "No MCC24 chart detected"
            return result
        gain = self.config_gain
        peak = _peak(requested_ms, gain)
        result.means = _means(min(peak, 4095.0))
        result.patch_status = [None] * 24
        if peak >= 4095:
            result.patch_status[18] = "clipped"
        result.polygons = np.zeros((24, 4, 2)) if polygons is None else polygons
        result.geometry = "detected" if polygons is None else "supplied"
        result.exposure_time_us, result.gain = requested_ms * 1000 + OFFSET_US, gain
        result.device_id, result.camera_ip, result.exposure_offset_us = DEVICE, "192.0.2.10", OFFSET_US
        result.loss = 1.5 if result.patch_status[18] is None else None
        return result

    async def test_bracket_captures_sequentially_and_suggests_the_ettr_exposure(self):
        series = await gox_exposure.run("MCC24", 50.0)
        self.assertTrue(series.ok, series.reason)
        self.assertEqual(self.captured, [25.0, 50.0, 100.0])
        self.assertEqual(series.patch, 19)
        self.assertAlmostEqual(series.fit.offset_dn, BLACK, places=3)
        expected = ((0.8 * 4095 - BLACK) / SLOPE - OFFSET_US) / 1000
        self.assertAlmostEqual(series.suggested_exposure_ms, round(expected, 1))
        self.assertEqual(series.camera_ip, "192.0.2.10")
        self.assertTrue(Path(series.directory).parent == self.root)
        document = json.loads((Path(series.directory) / "series.json").read_text(encoding="utf-8"))
        self.assertEqual(document["status"], "completed")
        self.assertEqual(document["suggestion"]["exposure_ms"], series.suggested_exposure_ms)
        self.assertEqual(len(document["points"]), 3)
        self.assertIs(gox_exposure.last_series(), series)

    async def test_frame_period_limit_caps_the_exposure_and_keeps_the_gain(self):
        self.limit.return_value = (180.0, ["config-gox.yaml: 5 Hz → exposure < 180 ms"])
        series = await gox_exposure.run("MCC24", 50.0)
        self.assertTrue(series.ok, series.reason)
        suggestion = series.suggestion
        self.assertEqual((suggestion.exposure_ms, suggestion.gain, suggestion.capped), (176.4, 1.0, True))
        self.assertIn("frame-period limit", suggestion.note)
        self.limit.assert_called_once_with("192.0.2.10")
        # Verifying at the capped exposure passes against the reachable peak, not the 80 % target.
        check = gox_exposure.check_reference("MCC24", self._provenance(176.4), "reference_capped")
        self.assertTrue(check.within, check.message)
        self.assertLess(check.target_dn, series.fit.target_dn)
        self.assertIsNone(check.suggestion)

    async def test_configured_gain_is_read_back_not_replaced(self):
        self.config_gain = 2.0
        series = await gox_exposure.run("MCC24", 50.0)
        self.assertTrue(series.ok, series.reason)
        self.assertEqual((series.gain, series.suggestion.gain), (2.0, 2.0))
        expected = ((0.8 * 4095 - BLACK) / SLOPE / 2.0 - OFFSET_US) / 1000
        self.assertAlmostEqual(series.suggested_exposure_ms, round(expected, 1))

    async def test_undetected_capture_reuses_geometry_from_the_same_scene(self):
        self.undetectable = {25.0}
        series = await gox_exposure.run("MCC24", 50.0)
        self.assertTrue(series.ok, series.reason)
        first = series.points[0]
        self.assertEqual(first.geometry, "supplied")
        self.assertIsNotNone(first.point)

    async def test_saturated_high_step_is_excluded(self):
        series = await gox_exposure.run("MCC24", 450.0)  # 400/450/500 ms; 500 ms clips
        self.assertTrue(series.ok, series.reason)
        self.assertEqual(series.points[2].point.status, "clipped")
        self.assertEqual((series.fit.points_used, series.fit.points_excluded), (2, 1))

    async def test_invalid_inputs_never_touch_the_camera(self):
        for chart, center in (("mcc24", 50), ("MCC24", 0), ("MCC24", -5)):
            with self.subTest(chart=chart, center=center), self.assertRaises(ValueError):
                await gox_exposure.run(chart, center)
        self.assertEqual(self.captured, [])

    def _provenance(self, exposure_ms: float, gain: float = 1.0, device=DEVICE):
        peak = _peak(exposure_ms, gain)
        return {"patch_rgb_normalized": _means(peak).tolist(), "source_full_scale_dn": 4095,
                "reference_exposure": {"device_id": device, "device_model": "GOX",
                                       "exposure_time_us": exposure_ms * 1000 + OFFSET_US, "gain": gain}}

    async def test_verification_within_tolerance_and_refit_outside(self):
        series = await gox_exposure.run("MCC24", 50.0)
        suggested = series.suggested_exposure_ms
        good = gox_exposure.check_reference("MCC24", self._provenance(suggested), "reference_a")
        self.assertTrue(good.within)
        self.assertLess(abs(good.deviation), exposure.TOLERANCE)
        self.assertIsNone(good.suggestion)
        self.assertIsNone(gox_exposure.last_series().suggestion)  # verified: nothing to apply
        # A capture 20 % long: outside tolerance, refit including it.
        bad = gox_exposure.check_reference("MCC24", self._provenance(suggested * 1.2), "reference_b")
        self.assertFalse(bad.within)
        self.assertAlmostEqual(bad.suggestion.exposure_ms, suggested, delta=1.0)
        self.assertEqual(len(gox_exposure.last_series().checks), 2)
        self.assertEqual(gox_exposure.last_series().suggestion, bad.suggestion)

    async def test_saturated_verification_gives_a_lower_exposure_bound(self):
        await gox_exposure.run("MCC24", 50.0)
        status = [None] * 24
        status[18] = "clipped"
        provenance = self._provenance(600.0)
        check = gox_exposure.check_reference("MCC24", provenance, "reference_c", status=status,
                                             means=np.asarray(provenance["patch_rgb_normalized"]))
        self.assertFalse(check.within)
        self.assertIsNone(check.deviation)
        self.assertLess(check.suggestion.exposure_ms, 600.0)

    async def test_verification_is_skipped_for_another_device_or_chart(self):
        await gox_exposure.run("MCC24", 50.0)
        self.assertIsNone(gox_exposure.check_reference("SG140", self._provenance(380), "x"))
        self.assertIsNone(gox_exposure.check_reference("MCC24", self._provenance(380, device="other"), "x"))
        self.assertIsNone(gox_exposure.check_reference("MCC24", {"reference_exposure": None}, "x"))


if __name__ == "__main__":
    unittest.main()
