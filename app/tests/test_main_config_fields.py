"""Offline contracts for Overview writeback; only temporary YAML files are used."""

from contextlib import ExitStack
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import yaml

from app.constants import ConfigFile
from app.services import config_store
from app.state import ProcState


MAIN = """# Main configuration
General:
    # Metadata
    Operator: 'Dev #1'   # keep this comment
    Field: "Test: west #2"
    Output Directory: ./data
    Enable ASTERX: true
    Enable FX10: false
    Enable GOX: true
    Enable LMS4XXX: false
Guards:
    Disk Min Free GiB: 5
"""


class MainConfigFieldTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.directory = Path(self.stack.enter_context(TemporaryDirectory()))
        self.path = self.directory / "config-main.yaml"
        self.path.write_text(MAIN, encoding="utf-8")
        self.state = SimpleNamespace(
            snapshot_busy=False, tool_uncertain=False, control_uncertain=False,
            config_locked=False, pending_config_notice=False, process_state=ProcState.IDLE,
        )
        self.stack.enter_context(patch.object(config_store, "STATE", self.state))
        self.stack.enter_context(patch.object(config_store, "get", return_value=ConfigFile("main", "Main", self.path)))
        self.require_owner = self.stack.enter_context(patch.object(config_store.control_owner, "require"))
        self.stack.enter_context(patch.dict(os.environ, {"AMIGA_DATA_ROOT": ""}))

    def contents(self):
        return self.path.read_text(encoding="utf-8")

    def test_strings_round_trip_without_changing_other_fields_or_comments(self):
        original = yaml.safe_load(MAIN)
        cases = ('Alice #3: "north"', "李四", "Field 🌾", "true", "123", "null", " leading and trailing ",
                 "back\\slash", "escaped\nline", "line\u2028separator", "line\u0085separator")
        for value in cases:
            with self.subTest(value=value):
                self.path.write_text(MAIN, encoding="utf-8")
                config_store.set_main_field("Operator", value, expected_value="Dev #1")
                saved = self.contents()
                parsed = yaml.safe_load(saved)
                self.assertIsInstance(parsed["General"]["Operator"], str)
                self.assertEqual(parsed["General"].pop("Operator"), value)
                expected = yaml.safe_load(MAIN)
                expected["General"].pop("Operator")
                self.assertEqual(parsed, expected)
                self.assertTrue(saved.startswith("# Main configuration\nGeneral:\n    # Metadata\n    Operator: "))
                self.assertTrue(saved.endswith(MAIN.split("   # keep this comment", 1)[1]))
                self.assertIn("   # keep this comment\n", saved)
                self.assertEqual(self.path.with_suffix(".yaml.bak").read_text(encoding="utf-8"), MAIN)
                self.assertEqual(config_store.main_settings()["operator"], value)
        self.assertEqual(original["General"]["Operator"], "Dev #1")

    def test_same_field_changed_elsewhere_is_not_overwritten(self):
        changed = MAIN.replace("'Dev #1'", "'Someone else'")
        self.path.write_text(changed, encoding="utf-8")
        with self.assertRaises(config_store.ConflictError):
            config_store.set_main_field("Operator", "New operator", expected_value="Dev #1")
        self.assertEqual(self.contents(), changed)
        self.assertFalse(self.path.with_suffix(".yaml.bak").exists())

    def test_other_fields_changed_elsewhere_are_retained(self):
        changed = MAIN.replace("./data", "./another-output").replace("Enable GOX: true", "Enable GOX: false")
        self.path.write_text(changed, encoding="utf-8")
        config_store.set_main_field("Operator", "New operator", expected_value="Dev #1")
        parsed = yaml.safe_load(self.contents())["General"]
        self.assertEqual(parsed["Operator"], "New operator")
        self.assertEqual(parsed["Output Directory"], "./another-output")
        self.assertFalse(parsed["Enable GOX"])
        self.assertEqual(self.path.with_suffix(".yaml.bak").read_text(encoding="utf-8"), changed)

    def test_batch_saves_all_fields_with_one_read_and_one_commit(self):
        values = {"Operator": "Alice #3", "Field": "East: 🌾", "Output Directory": "./next data"}
        expected = {"Operator": "Dev #1", "Field": "Test: west #2", "Output Directory": "./data"}
        with patch.object(config_store, "read", wraps=config_store.read) as read, \
                patch.object(config_store, "save", wraps=config_store.save) as save:
            config_store.set_main_fields(values, expected_values=expected)
        read.assert_called_once_with("main")
        self.assertEqual(save.call_count, 1)
        parsed = yaml.safe_load(self.contents())
        for key, value in values.items():
            self.assertEqual(parsed["General"][key], value)
        self.assertIn("   # keep this comment\n", self.contents())
        self.assertEqual(parsed["Guards"], yaml.safe_load(MAIN)["Guards"])
        self.assertEqual(self.path.with_suffix(".yaml.bak").read_text(encoding="utf-8"), MAIN)

    def test_batch_conflict_or_invalid_value_does_not_save_any_field(self):
        changed = MAIN.replace('"Test: west #2"', '"Someone else field"')
        cases = (
            (changed, {"Operator": "Alice", "Field": "East"},
             {"Operator": "Dev #1", "Field": "Test: west #2"}, config_store.ConflictError),
            (MAIN, {"Operator": "Alice", "Output Directory": ""},
             {"Operator": "Dev #1", "Output Directory": "./data"}, ValueError),
        )
        for text, values, expected, error in cases:
            with self.subTest(error=error):
                self.path.write_text(text, encoding="utf-8")
                with patch.object(config_store, "save", wraps=config_store.save) as save, \
                        self.assertRaises(error):
                    config_store.set_main_fields(values, expected_values=expected)
                save.assert_not_called()
                self.assertEqual(self.contents(), text)
                self.assertFalse(self.path.with_suffix(".yaml.bak").exists())

    def test_batch_preserves_external_changes_to_unsubmitted_fields(self):
        changed = MAIN.replace("./data", "./another-output").replace("Enable GOX: true", "Enable GOX: false")
        self.path.write_text(changed, encoding="utf-8")
        config_store.set_main_fields(
            {"Operator": "Alice", "Field": "East"},
            expected_values={"Operator": "Dev #1", "Field": "Test: west #2"},
        )
        parsed = yaml.safe_load(self.contents())["General"]
        self.assertEqual(parsed["Operator"], "Alice")
        self.assertEqual(parsed["Field"], "East")
        self.assertEqual(parsed["Output Directory"], "./another-output")
        self.assertFalse(parsed["Enable GOX"])
        self.assertEqual(self.path.with_suffix(".yaml.bak").read_text(encoding="utf-8"), changed)

    def test_matching_requested_value_is_a_no_op_even_with_an_older_baseline(self):
        before = self.path.stat().st_mtime_ns
        config_store.set_main_field("Operator", "Dev #1", expected_value="Earlier operator")
        self.assertEqual(self.contents(), MAIN)
        self.assertEqual(self.path.stat().st_mtime_ns, before)
        self.assertFalse(self.path.with_suffix(".yaml.bak").exists())

    def test_change_after_read_is_rejected_by_save(self):
        loaded = config_store.read("main")
        changed = MAIN.replace("./data", "./changed-after-read")
        self.path.write_text(changed, encoding="utf-8")
        stat = self.path.stat()
        os.utime(self.path, ns=(stat.st_atime_ns, loaded.mtime + 1_000_000))
        with patch.object(config_store, "read", return_value=loaded), self.assertRaises(config_store.ConflictError):
            config_store.set_main_field("Operator", "New operator", expected_value="Dev #1")
        self.assertEqual(self.contents(), changed)

    def test_blank_fields_and_control_characters_in_paths_are_rejected(self):
        for key in ("Operator", "Field", "Output Directory"):
            for value in ("", " \t "):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    config_store.set_main_field(key, value, expected_value="unused")
        for value in ("./data\nnew", "./data\x00", "./data\x7f", "./data\x85"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                config_store.set_main_field("Output Directory", value, expected_value="./data")
        self.assertEqual(self.contents(), MAIN)

    def test_output_path_respects_shared_root_and_does_not_create_a_directory(self):
        allowed = self.directory / "new recording directory"
        with patch.dict(os.environ, {"AMIGA_DATA_ROOT": str(self.directory)}):
            with self.assertRaises(ValueError):
                config_store.set_main_field("Output Directory", str(self.directory.parent / "outside"),
                                           expected_value="./data")
            config_store.set_main_field("Output Directory", str(allowed), expected_value="./data")
        self.assertFalse(allowed.exists())
        self.assertEqual(yaml.safe_load(self.contents())["General"]["Output Directory"], str(allowed))

    def test_unsafe_yaml_layouts_are_refused_without_writing(self):
        cases = (
            MAIN.replace("    Operator: 'Dev #1'", "    Operator: 'Dev #1'\n    Operator: Other"),
            MAIN + "General: {Operator: Other}\n",
            MAIN.replace("    Operator: 'Dev #1'   # keep this comment\n", ""),
            MAIN.replace("'Dev #1'", "&operator 'Dev #1'"),
            MAIN.replace("'Dev #1'", "[Dev]"),
            MAIN.replace("'Dev #1'   # keep this comment", "|\n        Dev #1"),
        )
        for text in cases:
            with self.subTest(text=text):
                self.path.write_text(text, encoding="utf-8")
                with self.assertRaises(config_store.ApplyError):
                    config_store.set_main_field("Operator", "New operator", expected_value="Dev #1")
                self.assertEqual(self.contents(), text)
                self.assertFalse(self.path.with_suffix(".yaml.bak").exists())

    def test_configuration_locks_and_control_owner_are_enforced(self):
        for lock in ("snapshot_busy", "tool_uncertain", "control_uncertain", "config_locked"):
            with self.subTest(lock=lock):
                setattr(self.state, lock, True)
                with self.assertRaises(config_store.ConfigBusyError):
                    config_store.set_main_field("Operator", "New operator", expected_value="Dev #1")
                setattr(self.state, lock, False)
                self.assertEqual(self.contents(), MAIN)
        self.require_owner.side_effect = RuntimeError("Not the control owner")
        with self.assertRaisesRegex(RuntimeError, "Not the control owner"):
            config_store.set_main_field("Operator", "New operator", expected_value="Dev #1")
        self.assertEqual(self.contents(), MAIN)

    def test_running_acquisition_gets_pending_configuration_notice(self):
        self.state.process_state = ProcState.RUNNING
        config_store.set_main_field("Field", "East field", expected_value="Test: west #2")
        self.assertTrue(self.state.pending_config_notice)

    def test_missing_metadata_uses_acquisition_defaults_for_display(self):
        self.path.write_text("General: {}\n", encoding="utf-8")
        settings = config_store.main_settings()
        self.assertEqual(settings["operator"], "Anonymous")
        self.assertEqual(settings["field"], "Unknown")
        self.assertEqual(settings["output_dir_raw"], "./data")

    def test_existing_unquoted_scalars_keep_their_yaml_cpp_text_spelling(self):
        text = MAIN.replace("'Dev #1'", "yes").replace('"Test: west #2"', "00123")
        self.path.write_text(text, encoding="utf-8")
        settings = config_store.main_settings()
        self.assertEqual(settings["operator"], "yes")
        self.assertEqual(settings["field"], "00123")
        config_store.set_main_field("Operator", "true", expected_value="yes")
        self.assertEqual(yaml.safe_load(self.contents())["General"]["Operator"], "true")
        self.assertIn("    Field: 00123\n", self.contents())

    def test_explicit_null_metadata_is_not_silently_coerced_to_text(self):
        self.path.write_text(MAIN.replace("'Dev #1'", "null"), encoding="utf-8")
        with self.assertRaises(ValueError):
            config_store.main_settings()
        with self.assertRaises(config_store.ApplyError):
            config_store.set_main_field("Operator", "Someone", expected_value="null")
