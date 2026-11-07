"""GOX reference contracts; camera execution and colour analysis are substituted.

These tests exercise temporary filesystem journals, never a detector or device.
"""

import json
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

from app.decoder.gox import color_checker
from app.services import gox_reference, gox_tools


def calibration(chart_key="MCC24"):
    chart = color_checker.CHARTS[chart_key]
    return color_checker.CalibrationPreview(
        chart_key=chart_key, chart_label=chart.label,
        patch_count=chart.patch_count, used_patches=chart.patch_count,
        matrix=[[1.1, -0.1, 0.0], [0.0, 1.0, 0.0], [0.0, -0.1, 1.1]],
        loss=0.25, original_png=b"original-preview",
        annotated_png=b"detected-preview", corrected_png=b"corrected-preview",
        pixel_format="BayerRG12Packed", full_scale=4095,
        provenance={"sampling": "native RGB", "frame_sequence": 7},
    )


class ReferenceValidationTests(unittest.IsolatedAsyncioTestCase):
    async def test_invalid_selection_never_reserves_or_touches_camera(self):
        with ExitStack() as stack:
            run = stack.enter_context(patch.object(gox_reference.camera_operations, "run",
                                                   new_callable=AsyncMock))
            prepare = stack.enter_context(patch.object(gox_reference, "_prepare"))
            capture = stack.enter_context(patch.object(gox_tools, "capture_raw", new_callable=AsyncMock))
            capability = stack.enter_context(patch.object(gox_reference, "capability_reason"))
            for chart_key in (None, "", "mcc24", "MCC25", 0, []):
                with self.subTest(chart_key=chart_key), self.assertRaises(ValueError):
                    await gox_reference.capture(chart_key)
            capability.assert_not_called()
            run.assert_not_awaited()
            prepare.assert_not_called()
            capture.assert_not_awaited()

    async def test_missing_opencv_capability_blocks_before_camera_reservation(self):
        with ExitStack() as stack:
            stack.enter_context(patch.object(gox_reference, "capability_reason",
                                             return_value="OpenCV mcc/ccm unavailable"))
            run = stack.enter_context(patch.object(gox_reference.camera_operations, "run",
                                                   new_callable=AsyncMock))
            prepare = stack.enter_context(patch.object(gox_reference, "_prepare"))
            capture = stack.enter_context(patch.object(gox_tools, "capture_raw", new_callable=AsyncMock))
            with self.assertRaisesRegex(ValueError, "mcc/ccm unavailable"):
                await gox_reference.capture("MCC24")
            run.assert_not_awaited()
            prepare.assert_not_called()
            capture.assert_not_awaited()

    async def test_each_explicit_chart_uses_shared_camera_reservation(self):
        async def reserve(sensor, operation, *, kind):
            self.assertEqual(sensor, "gox")
            self.assertEqual(kind, "gox-color-reference")
            return await operation()

        with patch.object(gox_reference, "capability_reason", return_value=None), \
                patch.object(gox_reference.camera_operations, "run", side_effect=reserve), \
                patch.object(gox_reference, "_capture", new_callable=AsyncMock) as capture:
            for chart_key in ("MCC24", "SG140", "VINYL18"):
                with self.subTest(chart_key=chart_key):
                    await gox_reference.capture(chart_key)
                    capture.assert_awaited_with(chart_key)


class ReferenceCaptureTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "recordings"
        self.output.mkdir()
        self.config = self.root / "config-gox-snapshot.yaml"
        self.config_bytes = b"# Preserve original config bytes\ndevice:\n  ip: 192.0.2.10\n"
        self.config.write_bytes(self.config_bytes)
        self.raw_bytes = b"stand-in RAW bytes; never decoded by these service tests"
        self.raw_files = []
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(gox_reference, "_last_result", None))
        self.stack.enter_context(patch.object(gox_reference, "_status", ""))
        self.stack.enter_context(patch.object(gox_reference, "SNAPSHOT_CONFIG", self.config))
        self.stack.enter_context(patch.object(gox_reference.config_store, "checked_snapshot_path",
                                              return_value=self.config))
        self.stack.enter_context(patch.object(gox_reference.config_store, "main_settings", return_value={
            "output_dir_raw": str(self.output), "output_dir": self.output,
        }))
        self.problems = self.stack.enter_context(patch.object(
            gox_reference.config_store, "output_dir_problems", return_value=[]))
        self.stack.enter_context(patch.object(gox_reference, "capability_reason", return_value=None))
        self.stack.enter_context(patch.object(gox_reference.camera_operations, "run", side_effect=self.reserve))
        self.capture = self.stack.enter_context(patch.object(
            gox_tools, "capture_raw", side_effect=self.capture_raw))
        self.analyse = self.stack.enter_context(patch.object(
            color_checker, "calibrate", side_effect=lambda _directory, chart_key: calibration(chart_key)))
        self.cleanup = self.stack.enter_context(patch.object(gox_tools, "_cleanup_old"))
        self.snapshot = self.stack.enter_context(patch.object(gox_tools, "_snapshot", new_callable=AsyncMock))
        self.spawn = self.stack.enter_context(patch.object(gox_tools.tool_jobs, "spawn", new_callable=AsyncMock))

    async def reserve(self, sensor, operation, *, kind):
        self.assertEqual((sensor, kind), ("gox", "gox-color-reference"))
        return await operation()

    async def capture_raw(self, request, output, *, config):
        self.assertIsNone(request)
        self.assertEqual(config.read_bytes(), self.config_bytes)
        self.assertEqual(config.parent.parent, output.parent)
        self.assertEqual(output.name, "gox")
        camera = output / "camera0"
        camera.mkdir(parents=True)
        raw = camera / "segment_000000.raw"
        raw.write_bytes(self.raw_bytes)
        (camera / "device.json").write_text('{"device_kind": "gox"}', encoding="utf-8")
        self.raw_files.append(raw)
        return gox_tools.RawCapture(True, camera_dir=camera, raw_output="captured one frame")

    async def test_reference_keeps_raw_config_and_relative_calibration_metadata(self):
        result = await gox_reference.capture("SG140")
        self.assertTrue(result.ok, result.reason)
        session = Path(result.session_dir)
        self.assertEqual(session.parent, self.output / "tmp")
        self.assertTrue(session.name.startswith("reference_gox_"))
        document = json.loads((session / "reference.json").read_text(encoding="utf-8"))
        self.assertEqual(document["format"], "gox-color-reference-v1")
        self.assertEqual(document["status"], "completed")
        self.assertEqual(document["chart_type"], "SG140")
        self.assertEqual(document["target_color_space"], "sRGB")
        self.assertEqual(document["target_white_point"], "D65")
        self.assertEqual(document["camera_directory"], "raw/gox/camera0")
        self.assertEqual(document["device_metadata"], "raw/gox/camera0/device.json")
        self.assertEqual(document["config_snapshot"], "raw/config/config-gox-snapshot.yaml")
        self.assertEqual(document["calibration"]["matrix"], calibration("SG140").matrix)
        self.assertEqual(document["calibration"]["patch_count"], 140)
        self.assertEqual(document["calibration"]["full_scale"], 4095)
        self.assertNotIn("request", document)
        self.assertEqual(document["config_source"], str(self.config))
        self.assertEqual(result.config_snapshot, str(session / document["config_snapshot"]))
        self.assertEqual((session / document["config_snapshot"]).read_bytes(), self.config_bytes)
        self.assertEqual(self.config.read_bytes(), self.config_bytes)
        self.assertEqual(self.raw_files[0].read_bytes(), self.raw_bytes)
        self.assertIs(gox_reference.last_result(), result)
        self.analyse.assert_called_once_with(session / "raw/gox/camera0", "SG140")
        self.cleanup.assert_not_called()
        self.snapshot.assert_not_awaited()
        self.spawn.assert_not_awaited()
        self.assertEqual(sorted(path.suffix for path in session.rglob("*") if path.is_file()),
                         [".json", ".json", ".raw", ".yaml"])

    async def test_saving_moves_the_reference_from_tmp_into_the_output_directory(self):
        result = await gox_reference.capture("MCC24")
        self.assertTrue(result.ok, result.reason)
        self.assertFalse(result.saved)
        tmp_session = Path(result.session_dir)
        saved = await gox_reference.save_to_data()
        target = self.output / tmp_session.name
        self.assertTrue(saved.saved)
        self.assertEqual(Path(saved.session_dir), target)
        self.assertFalse(tmp_session.exists())
        self.assertEqual((target / "raw/gox/camera0/segment_000000.raw").read_bytes(), self.raw_bytes)
        document = json.loads((target / "reference.json").read_text(encoding="utf-8"))
        self.assertEqual(document["status"], "completed")
        self.assertEqual(document["saved_from"], f"tmp/{tmp_session.name}")
        self.assertEqual(saved.config_snapshot, str(target / document["config_snapshot"]))
        self.assertIs(gox_reference.last_result(), saved)
        with self.assertRaisesRegex(ValueError, "already saved"):
            await gox_reference.save_to_data()

    async def test_failed_reference_cannot_be_saved(self):
        self.analyse.side_effect = ValueError("No MCC24 chart detected")
        await gox_reference.capture("MCC24")
        with self.assertRaisesRegex(ValueError, "no completed reference"):
            await gox_reference.save_to_data()

    async def test_detection_or_fit_failure_is_failed_and_preserves_raw(self):
        failures = (
            (ValueError("No MCC24 chart detected"), "No MCC24 chart detected"),
            (ValueError("Color correction fit is not finite"), "Color correction fit is not finite"),
            (ValueError(), "ValueError"),
        )
        for error, message in failures:
            with self.subTest(failure=message):
                self.analyse.side_effect = error
                result = await gox_reference.capture("MCC24")
                self.assertFalse(result.ok)
                self.assertEqual(result.reason, message)
                self.assertIsNone(result.analysis)
                document = json.loads((Path(result.session_dir) / "reference.json").read_text(encoding="utf-8"))
                self.assertEqual(document["status"], "failed")
                self.assertEqual(document["error"], message)
                self.assertNotIn("calibration", document)
                self.assertEqual(self.raw_files[-1].read_bytes(), self.raw_bytes)
                self.assertIn("failed", gox_reference.status())
        self.cleanup.assert_not_called()
        self.snapshot.assert_not_awaited()
        self.spawn.assert_not_awaited()

    async def test_initial_file_failure_reports_created_session_without_acquiring(self):
        with patch.object(gox_reference, "_initialize", side_effect=OSError("Cannot save snapshot config")):
            result = await gox_reference.capture("MCC24")
        self.assertFalse(result.ok)
        self.assertEqual(result.reason, "Cannot save snapshot config")
        session = Path(result.session_dir)
        self.assertEqual(session.parent, self.output / "tmp")
        self.assertTrue(session.is_dir())
        document = json.loads((session / "reference.json").read_text(encoding="utf-8"))
        self.assertEqual(document["status"], "failed")
        self.assertEqual(document["error"], result.reason)
        self.assertNotIn("calibration", document)
        self.assertNotIn("completed_utc", document)
        self.capture.assert_not_awaited()
        self.analyse.assert_not_called()
        self.cleanup.assert_not_called()
        self.spawn.assert_not_awaited()

    async def test_output_validation_failure_precedes_config_snapshot_and_capture(self):
        self.problems.return_value = ["Output directory is outside the allowed root"]
        with patch.object(gox_reference, "_config_bytes") as config_bytes:
            result = await gox_reference.capture("VINYL18")
        self.assertFalse(result.ok)
        self.assertIn("outside the allowed root", result.reason)
        self.assertEqual(result.session_dir, "")
        self.assertEqual(list(self.output.iterdir()), [])
        config_bytes.assert_not_called()
        self.capture.assert_not_awaited()
        self.analyse.assert_not_called()

    async def test_config_size_limit_prevents_creating_reference_output(self):
        with patch.object(gox_reference, "MAX_CONFIG_BYTES", len(self.config_bytes) - 1):
            result = await gox_reference.capture("MCC24")
        self.assertFalse(result.ok)
        self.assertIn("configuration exceeds", result.reason)
        self.assertEqual(list(self.output.iterdir()), [])
        self.capture.assert_not_awaited()

    async def test_tool_failure_does_not_publish_calibration_or_remove_partial_raw(self):
        async def failed_capture(request, output, *, config):
            captured = await self.capture_raw(request, output, config=config)
            return gox_tools.RawCapture(False, reason="Capture was incomplete",
                                        raw_output=captured.raw_output)

        self.capture.side_effect = failed_capture
        result = await gox_reference.capture("MCC24")
        self.assertFalse(result.ok)
        document = json.loads((Path(result.session_dir) / "reference.json").read_text(encoding="utf-8"))
        self.assertEqual(document["status"], "failed")
        self.assertNotIn("calibration", document)
        self.assertEqual(self.raw_files[0].read_bytes(), self.raw_bytes)
        self.analyse.assert_not_called()
        self.cleanup.assert_not_called()


class RawCapturePathTests(unittest.IsolatedAsyncioTestCase):
    async def test_reference_uses_saved_config_without_camera_parameter_overrides(self):
        output = Path("/reference/raw/gox")
        config = Path("/reference/raw/config/config-gox-snapshot.yaml")
        process = SimpleNamespace(returncode=0)
        with ExitStack() as stack:
            spawn = stack.enter_context(patch.object(gox_tools.tool_jobs, "spawn",
                                                     new_callable=AsyncMock, return_value=process))
            stack.enter_context(patch.object(gox_tools.tool_jobs, "communicate", new_callable=AsyncMock,
                                             return_value=(b"SNAPSHOT: OK /reference/raw/gox/camera0\n", b"")))
            stack.enter_context(patch.object(gox_tools.runtime, "exec_path", side_effect=str))
            stack.enter_context(patch.object(gox_tools.runtime, "to_host_path", side_effect=Path))
            result = await gox_tools.capture_raw(None, output, config=config)
        self.assertTrue(result.ok, result.reason)
        arguments = spawn.await_args.args[0]
        self.assertEqual(arguments[arguments.index("--config") + 1], str(config))
        self.assertEqual(arguments[arguments.index("--out") + 1], str(output))
        for option in ("--ip", "--exposure-ms", "--gain"):
            self.assertNotIn(option, arguments)

    async def test_empty_or_unknown_success_marker_is_rejected_before_resolving_path(self):
        process = SimpleNamespace(returncode=0)
        with ExitStack() as stack:
            stack.enter_context(patch.object(gox_tools.tool_jobs, "spawn",
                                             new_callable=AsyncMock, return_value=process))
            communicate = stack.enter_context(patch.object(gox_tools.tool_jobs, "communicate",
                                                           new_callable=AsyncMock))
            stack.enter_context(patch.object(gox_tools.runtime, "exec_path", side_effect=str))
            to_host = stack.enter_context(patch.object(gox_tools.runtime, "to_host_path"))
            for marker in ("SNAPSHOT: OK", "SNAPSHOT: OK ", "SNAPSHOT: OK \t",
                           "SNAPSHOT: UNKNOWN /untrusted/camera0", "SNAPSHOT: OKAY /untrusted/camera0"):
                communicate.return_value = (f"{marker}\n".encode(), b"")
                with self.subTest(marker=marker):
                    result = await gox_tools.capture_raw(
                        gox_tools.SnapshotRequest("192.0.2.10", 10.0, 1.0), Path("/reference/raw/gox"))
                    self.assertFalse(result.ok)
                    self.assertEqual(result.reason, "Malformed SNAPSHOT result marker")
                    self.assertIsNone(result.camera_dir)
            to_host.assert_not_called()

    async def test_cloned_config_is_passed_to_tool_and_only_direct_camera_children_are_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "reference/raw/gox"
            config = root / "reference/raw/config/config-gox-snapshot.yaml"
            request = gox_tools.SnapshotRequest("192.0.2.10", 10.0, 1.0)
            process = SimpleNamespace(returncode=0)
            with ExitStack() as stack:
                spawn = stack.enter_context(patch.object(gox_tools.tool_jobs, "spawn",
                                                         new_callable=AsyncMock, return_value=process))
                communicate = stack.enter_context(patch.object(gox_tools.tool_jobs, "communicate",
                                                               new_callable=AsyncMock))
                stack.enter_context(patch.object(gox_tools.runtime, "exec_path", side_effect=str))
                stack.enter_context(patch.object(gox_tools.runtime, "to_host_path", side_effect=Path))
                cleanup = stack.enter_context(patch.object(gox_tools, "_cleanup_old"))
                for camera, accepted in ((output / "camera0", True),
                                         (output.parent / "other/camera0", False),
                                         (output / "../elsewhere/camera0", False),
                                         (output / "nested/camera0", False)):
                    communicate.return_value = (f"SNAPSHOT: OK {camera}\n".encode(), b"")
                    with self.subTest(camera=camera):
                        result = await gox_tools.capture_raw(request, output, config=config)
                        self.assertEqual(result.ok, accepted)
                        if accepted:
                            self.assertEqual(result.camera_dir, camera.resolve())
                        else:
                            self.assertIn("outside the requested camera output directory", result.reason)
                arguments = spawn.await_args.args[0]
                self.assertEqual(arguments[arguments.index("--config") + 1], str(config))
                self.assertEqual(arguments[arguments.index("--out") + 1], str(output))
                self.assertEqual(arguments[arguments.index("--ip") + 1], request.target_ip)
                cleanup.assert_not_called()


if __name__ == "__main__":
    unittest.main()
