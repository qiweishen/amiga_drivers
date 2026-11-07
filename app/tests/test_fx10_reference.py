"""FX10 reference configuration routing; no acquisition process or device."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

from app.services import fx10_reference


class ReferenceConfigTests(unittest.TestCase):
    def test_duration_comes_from_snapshot_profile_without_reading_recording_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            snapshot = root / "config-fx10-snapshot.yaml"
            recording = root / "config-fx10.yaml"
            snapshot.write_text("reference:\n  duration_s: 2.5\n", encoding="utf-8")
            recording.write_text("reference:\n  duration_s: 99\n", encoding="utf-8")
            with patch.object(fx10_reference, "FX10_SNAPSHOT_CONFIG", snapshot), \
                    patch.object(fx10_reference.config_store, "checked_snapshot_path", side_effect=lambda p: p.resolve()), \
                    patch.object(fx10_reference.config_store, "read") as read:
                path, duration = fx10_reference.configured_duration()
            self.assertEqual(path, snapshot.resolve())
            self.assertEqual(duration, 2.5)
            read.assert_not_called()

    def test_missing_duration_leaves_default_to_the_collector(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary) / "config-fx10-snapshot.yaml"
            snapshot.write_text("acquisition:\n  exposure_ms: 5\n", encoding="utf-8")
            with patch.object(fx10_reference, "FX10_SNAPSHOT_CONFIG", snapshot), \
                    patch.object(fx10_reference.config_store, "checked_snapshot_path", side_effect=lambda p: p.resolve()):
                self.assertEqual(fx10_reference.configured_duration(), (snapshot.resolve(), None))


class ReferenceCommandTests(unittest.IsolatedAsyncioTestCase):
    async def test_reference_uses_snapshot_path_without_trigger_or_workbench_overrides(self):
        snapshot = Path("/profiles/config-fx10-snapshot.yaml")
        with patch.object(fx10_reference, "FX10_SNAPSHOT_CONFIG", snapshot), \
                patch.object(fx10_reference, "_status", ""), \
                patch.object(fx10_reference, "_last_result", None), \
                patch.object(fx10_reference.runtime, "binary_exists", new_callable=AsyncMock, return_value=True), \
                patch.object(fx10_reference.runtime, "pgrep", new_callable=AsyncMock, return_value=False), \
                patch.object(fx10_reference.runtime, "exec_path", side_effect=str), \
                patch.object(fx10_reference.config_store, "checked_snapshot_path", return_value=snapshot) as checked, \
                patch.object(fx10_reference.reference_storage, "tmp_root", return_value=Path("/captures/tmp")), \
                patch.object(fx10_reference.config_store, "get") as get, \
                patch.object(fx10_reference.tool_jobs, "spawn", new_callable=AsyncMock,
                             side_effect=RuntimeError("capture substituted")) as spawn:
            result = await fx10_reference._collect()
        checked.assert_called_once_with(snapshot)
        get.assert_not_called()
        spawn.assert_awaited_once_with([
            str(fx10_reference.BIN_FX10_REFERENCE), "--config", str(snapshot), "--out", "/captures/tmp",
        ])
        self.assertFalse(result.ok)
        self.assertEqual(result.reason, "capture substituted")


class ReferenceSaveTests(unittest.IsolatedAsyncioTestCase):
    async def test_save_moves_the_pair_out_of_tmp_and_records_it_beside_the_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            session = root / "tmp" / "reference_20261106T000000Z_1"
            (session / "raw" / "fx10" / "white").mkdir(parents=True)
            (session / "reference.json").write_text('{"format": "fx10-reference-v1"}', encoding="utf-8")
            done = fx10_reference.ReferenceResult(True, session_dir=str(session))
            with patch.object(fx10_reference, "_last_result", done), \
                    patch.object(fx10_reference.reference_storage, "output_root", return_value=root):
                saved = await fx10_reference.save_to_data()
                self.assertTrue(saved.saved)
                self.assertEqual(Path(saved.session_dir), root / session.name)
                self.assertFalse(session.exists())
                # The collector's manifest is not rewritten.
                self.assertEqual((root / session.name / "reference.json").read_text(encoding="utf-8"),
                                 '{"format": "fx10-reference-v1"}')
                self.assertIn("saved_from", (root / session.name / "saved.json").read_text(encoding="utf-8"))
                with self.assertRaisesRegex(ValueError, "already saved"):
                    await fx10_reference.save_to_data()

    async def test_only_tmp_references_are_saved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            session = root / "reference_elsewhere"
            session.mkdir()
            with patch.object(fx10_reference, "_last_result",
                              fx10_reference.ReferenceResult(True, session_dir=str(session))), \
                    patch.object(fx10_reference.reference_storage, "output_root", return_value=root), \
                    self.assertRaisesRegex(ValueError, "Only references under"):
                await fx10_reference.save_to_data()
            self.assertTrue(session.exists())


if __name__ == "__main__":
    unittest.main()
