"""FX10 white-reference ETTR: bracket, band metric, series orchestration and verification; no camera."""

import json
import math
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch

from app.decoder.fx10 import exposure as fx10_exposure
from app.decoder.gox import exposure
from app.services import fx10_exposure as service, fx10_tools

BLACK, SLOPE = 160.0, 0.12  # DN, DN per µs (≈1360 DN at 10 ms)
FULL_SCALE, BANDS, BRIGHT = 4095, 8, 5
DEVICE = "FX10-0042"


def _p90(exposure_ms: float) -> list[float]:
    peak = BLACK + SLOPE * exposure_ms * 1000
    shape = [0.3, 0.5, 0.7, 0.9, 1.0, 0.8, 0.6, 0.4]  # band 5 brightest
    return [min(float(FULL_SCALE), BLACK + (peak - BLACK) * weight) for weight in shape]


def _device(exposure_ms: float, *, device=DEVICE, spatial=1, spectral=2) -> dict:
    def ok(value):
        return {"value": value, "status": "ok"}
    return {"kind": "fx10",
            "identity": {"DeviceSerialNumber": ok(device), "DeviceModelName": ok("FX10e")},
            "final_readback": {"ExposureTime": ok(exposure_ms * 1000.0), "BinningHorizontal": ok(spatial),
                               "BinningVertical": ok(spectral)}}


class ArithmeticTests(unittest.TestCase):
    def test_bracket_is_center_plus_minus_15_ms_or_half_center_below(self):
        self.assertEqual(fx10_exposure.bracket(40), [25.0, 40.0, 55.0])
        self.assertEqual(fx10_exposure.bracket(15), [7.5, 15.0, 30.0])
        self.assertEqual(fx10_exposure.bracket(10), [5.0, 10.0, 25.0])
        with self.assertRaises(ValueError):
            fx10_exposure.bracket(0)

    def test_target_fraction_defaults_to_60_percent_and_is_bounded(self):
        self.assertEqual(fx10_exposure.DEFAULT_TARGET_FRACTION, 0.60)
        self.assertEqual(fx10_exposure.target_fraction(0.7), 0.7)
        for invalid in (0.05, 0.99, math.nan, True, "0.6"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                fx10_exposure.target_fraction(invalid)

    def test_band_metric_and_saturation_status(self):
        p90 = _p90(10)
        self.assertEqual(fx10_exposure.brightest_band(p90), BRIGHT)
        point = fx10_exposure.point_from_spectrum("x", p90, BRIGHT, 10000.0, FULL_SCALE, [400 + 50 * i for i in range(8)])
        self.assertEqual((point.patch, point.gain, point.status), (BRIGHT, 1.0, None))
        self.assertIn("600.0 nm", point.peak_channel)
        self.assertEqual(fx10_exposure.band_status(4095, 4095), "clipped")
        self.assertEqual(fx10_exposure.band_status(4040, 4095), "near_saturation")
        self.assertIsNone(fx10_exposure.band_status(4000, 4095))

    def test_readback_is_required_and_never_defaulted(self):
        readback = fx10_exposure.exposure_from_device(_device(12.5))
        self.assertEqual((readback["exposure_time_us"], readback["spatial_binning"], readback["spectral_binning"]),
                         (12500.0, 1, 2))
        missing = _device(12.5)
        missing["final_readback"]["ExposureTime"] = {"value": None, "status": "unreadable"}
        with self.assertRaisesRegex(ValueError, "ExposureTime"):
            fx10_exposure.exposure_from_device(missing)
        with self.assertRaises(ValueError):
            fx10_exposure.exposure_from_device({**_device(1), "kind": "gox"})


class SeriesTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "tmp"
        self.captured = []
        self.devices = {}
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(service, "_series", None))
        self.stack.enter_context(patch.object(service.reference_storage, "tmp_root", return_value=self.root))
        self.stack.enter_context(patch.object(service, "_config_bytes", return_value=b"acquisition: {}\n"))
        self.stack.enter_context(patch.object(service.camera_operations, "run", side_effect=self.reserve))
        self.stack.enter_context(patch.object(fx10_tools, "capture_raw", side_effect=self.capture_raw))
        self.stack.enter_context(patch.object(service, "_analyze", side_effect=self.fake_analyze))
        self.stack.enter_context(patch.object(service.raw_recording, "device_metadata",
                                              side_effect=lambda directory: self.devices[Path(directory)]))
        self.limit = self.stack.enter_context(patch.object(service, "_exposure_limit", return_value=(None, [])))

    async def reserve(self, sensor, operation, *, kind):
        self.assertEqual((sensor, kind), ("fx10", "fx10-exposure-series"))
        return await operation()

    async def capture_raw(self, output, exposure_ms, *, config):
        self.assertEqual(config.read_bytes(), b"acquisition: {}\n")
        session = output / "fx10_session"
        session.mkdir(parents=True)
        self.captured.append(exposure_ms)
        return fx10_tools.SnapshotResult(True, session_dir=str(session), raw_output=f"captured {exposure_ms} ms")

    def fake_analyze(self, session_dir, requested_ms):
        result = service._Analysis(session_dir, requested_ms)
        result.readback = fx10_exposure.exposure_from_device(_device(requested_ms))
        result.p90, result.full_scale, result.bands = _p90(requested_ms), FULL_SCALE, BANDS
        result.mean_dn, result.clipped_pct = 1000.0, 0.0
        return result

    def white(self, exposure_ms: float, **device) -> Path:
        directory = self.root / f"white_{exposure_ms:g}"
        self.devices[directory] = _device(exposure_ms, **device)
        return directory

    async def test_bracket_captures_sequentially_and_suggests_the_60_percent_exposure(self):
        series = await service.run(10.0)
        self.assertTrue(series.ok, series.reason)
        self.assertEqual(self.captured, [5.0, 10.0, 25.0])
        self.assertEqual((series.band, series.binning, series.target_fraction), (BRIGHT, (1, 2), 0.60))
        self.assertAlmostEqual(series.fit.offset_dn, BLACK, places=3)
        expected = (0.6 * FULL_SCALE - BLACK) / SLOPE / 1000
        self.assertAlmostEqual(series.suggested_exposure_ms, round(expected, 1))
        self.assertEqual(Path(series.directory).parent, self.root)
        document = json.loads((Path(series.directory) / "series.json").read_text(encoding="utf-8"))
        self.assertEqual((document["status"], document["target_fraction"]), ("completed", 0.6))

    async def test_user_target_fraction_and_saturated_step(self):
        series = await service.run(25.0, 0.8)  # 10/25/40 ms; 40 ms saturates band 5
        self.assertTrue(series.ok, series.reason)
        self.assertEqual(series.points[2].point.status, "clipped")
        self.assertEqual((series.fit.points_used, series.fit.points_excluded), (2, 1))
        self.assertAlmostEqual(series.fit.target_dn, 0.8 * FULL_SCALE)

    async def test_frame_period_limit_caps_the_exposure_and_keeps_binning(self):
        self.limit.return_value = (900.0 / 51.0, ["config-fx10.yaml: 51 Hz → exposure < 17.6471 ms"])
        series = await service.run(10.0)
        suggestion = series.suggestion
        self.assertTrue(suggestion.capped)
        self.assertLess(suggestion.exposure_ms, 900.0 / 51.0)
        self.assertIn("Binning is unchanged", suggestion.note)
        self.assertNotIn("gain", suggestion.note.lower())  # the FX10 note never advises a gain
        check = service.check_reference(self.white(suggestion.exposure_ms), _p90(suggestion.exposure_ms),
                                        FULL_SCALE, "reference/white")
        self.assertTrue(check.within, check.message)
        self.assertLess(check.target_dn, series.fit.target_dn)

    async def test_verification_within_tolerance_and_refit_outside(self):
        series = await service.run(10.0)
        suggested = series.suggested_exposure_ms
        good = service.check_reference(self.white(suggested), _p90(suggested), FULL_SCALE, "a/white")
        self.assertTrue(good.within)
        self.assertIsNone(service.last_series().suggestion)
        long = suggested * 1.2
        bad = service.check_reference(self.white(long), _p90(long), FULL_SCALE, "b/white")
        self.assertFalse(bad.within)
        self.assertAlmostEqual(bad.suggestion.exposure_ms, suggested, delta=0.2)
        # The refit keeps the series' 60 % target, not the GOX default.
        self.assertAlmostEqual(service.last_series().checks[-1].target_dn, 0.6 * FULL_SCALE)

    async def test_saturated_white_reference_gives_a_lower_exposure(self):
        await service.run(10.0)
        check = service.check_reference(self.white(60.0), _p90(60.0), FULL_SCALE, "c/white")
        self.assertFalse(check.within)
        self.assertIsNone(check.deviation)
        self.assertLess(check.suggestion.exposure_ms, 60.0)

    async def test_verification_is_skipped_for_another_camera_or_binning(self):
        await service.run(10.0)
        self.assertIsNone(service.check_reference(self.white(20, device="other"), _p90(20), FULL_SCALE, "x"))
        self.assertIsNone(service.check_reference(self.white(21, spectral=4), _p90(21), FULL_SCALE, "x"))
        self.assertIsNone(service.check_reference(self.white(22), _p90(22)[:4], FULL_SCALE, "x"))

    async def test_invalid_inputs_never_touch_the_camera(self):
        for center, fraction in ((0, 0.6), (-1, 0.6), (10, 0.01), (10, 1.2)):
            with self.subTest(center=center, fraction=fraction), self.assertRaises(ValueError):
                await service.run(center, fraction)
        self.assertEqual(self.captured, [])


if __name__ == "__main__":
    unittest.main()
