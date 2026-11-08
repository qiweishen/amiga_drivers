"""FX10 workbench planning contracts; no snapshot tool or camera is run."""

import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

from app.services import fx10_tools


class SnapshotPlanningTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.config = Path(temporary.name) / "config-fx10-snapshot.yaml"
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(fx10_tools, "FX10_SNAPSHOT_CONFIG", self.config))
        self.stack.enter_context(patch.object(fx10_tools.config_store, "checked_snapshot_path",
                                              side_effect=lambda path: path))

    def test_config_rate_lowered_to_fit_the_exposure_determines_frame_count_and_deadline(self):
        for yaml_rate, exposure_ms, expected_rate, expected_frames, expected_timeout in (
            ("frame_rate_hz: 0.1", 10, 0.1, 1, 40),
            ("frame_rate_hz: 5", 10, 5, 5, 13),
            ("frame_rate_hz: 100", 200, 4.5, 4, 4 / 4.5 * 3 + 10),  # 90 % of the period: 4.5 Hz
            ("{}", 10, 50, 50, 13),
        ):
            with self.subTest(rate=yaml_rate, exposure_ms=exposure_ms):
                self.config.write_text(f"acquisition: {{ {yaml_rate} }}\n" if yaml_rate != "{}"
                                       else "acquisition: {}\n", encoding="utf-8")
                config, rate, frames, timeout = fx10_tools._capture_plan(exposure_ms)
                self.assertEqual(config, self.config)
                self.assertEqual((rate, frames), (expected_rate, expected_frames))
                self.assertAlmostEqual(timeout, expected_timeout)

    def test_invalid_or_impractically_slow_rate_is_rejected_before_spawning(self):
        for value in ("0", "-1", ".nan", ".inf", "true", "wrong", "0.000001"):
            with self.subTest(rate=value):
                self.config.write_text(f"acquisition:\n  frame_rate_hz: {value}\n", encoding="utf-8")
                with self.assertRaises(ValueError):
                    fx10_tools._capture_plan(10)

    async def test_command_and_timeout_use_the_same_configured_rate(self):
        self.config.write_text("acquisition:\n  frame_rate_hz: 0.1\n", encoding="utf-8")
        with ExitStack() as stack:
            stack.enter_context(patch.object(fx10_tools.runtime, "exec_path", side_effect=str))
            stack.enter_context(patch.object(fx10_tools, "FX10_SNAPSHOT_DIR", self.config.parent / "captures"))
            spawn = stack.enter_context(patch.object(fx10_tools.tool_jobs, "spawn", new_callable=AsyncMock,
                                                     return_value=SimpleNamespace(returncode=2)))
            communicate = stack.enter_context(patch.object(fx10_tools.tool_jobs, "communicate",
                                                           new_callable=AsyncMock, return_value=(b"", b"")))
            await fx10_tools._snapshot("192.0.2.10", 10, 2, 4)
        arguments = spawn.await_args.args[0]
        for option, value in (("--config", str(self.config)), ("--fps", "0.1"), ("--frames", "1"),
                              ("--exposure-ms", "10"), ("--spatial-binning", "2"), ("--spectral-binning", "4")):
            self.assertEqual(arguments[arguments.index(option) + 1], value)
        self.assertEqual(communicate.await_args.args[1], 40)

    def test_workbench_rate_is_used_as_given_and_checked_against_binning_and_exposure(self):
        self.config.write_text("acquisition:\n  frame_rate_hz: 50\n  spatial_binning: 1\n  spectral_binning: 2\n",
                               encoding="utf-8")
        _config, rate, frames, _timeout = fx10_tools._capture_plan(2.0, frame_rate_hz=300.0)
        self.assertEqual((rate, frames), (300.0, 300))
        for kwargs in ({"frame_rate_hz": 324.374},  # above the binned ceiling
                       {"frame_rate_hz": 200.0, "spatial_binning": 1, "spectral_binning": 1},  # > 163.02 Hz
                       {"frame_rate_hz": 460.0, "spatial_binning": 2}):  # 2 ms > 90 % of the period
            with self.subTest(**kwargs), self.assertRaises(ValueError):
                fx10_tools._capture_plan(2.0, **kwargs)
        with self.assertRaises(ValueError):
            fx10_tools._capture_plan(419.001)  # ExposureTime maximum
