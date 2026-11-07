"""Camera Apply contracts for formal and snapshot YAML; no devices are used."""

import os
import unittest
from contextlib import ExitStack
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

import yaml

from app.constants import ConfigFile
from app.services import config_store
from app.state import ProcState


TARGET_IP = "192.0.2.10"
TARGET_MAC = "00:11:22:33:44:55"


def gox_document(*, reverse=False, mode="external", rate=5.0, mac=TARGET_MAC):
    target = f"""  - id: selected
    device:
      ip: {TARGET_IP}
      mac: '{mac}'
    acquisition:
      exposure_ms: 10.0    # selected exposure comment
      gain: 1.0           # selected gain comment
      frame_rate_hz: {rate}
      pixel_format: BayerRG12Packed
      trigger:
        mode: {mode}
        exposure_active_output: false
"""
    unrelated = """  - id: untouched
    device:
      ip: 192.0.2.11
      mac: '00:11:22:33:44:66'
    acquisition:
      exposure_ms: 6.0
      gain: 3.0
      frame_rate_hz: 5.0
      pixel_format: BayerRG12Packed
      trigger:
        mode: external
"""
    cameras = target + unrelated if reverse else unrelated + target
    return ("# GOX timing and camera identity must survive Apply\ncameras:\n" + cameras +
            "ptp:\n  enabled: true\nsensor_trigger:\n  enabled: true\n"
            "output:\n  segment_size_mb: 1024\n")


def fx10_document(*, mode="external", mroi=False, rate=50.0):
    rate_line = f"  frame_rate_hz: {rate}\n" if rate is not None else ""
    return f"""# FX10 timing and output must survive Apply
device:
  ip: 192.0.2.20
acquisition:
  exposure_ms: 5.0          # keep exposure documentation
  spatial_binning: 1       # keep spatial documentation
  spectral_binning: 1      # keep spectral documentation
  pixel_format: Mono12Packed
{rate_line}  trigger:
    mode: {mode}
  mroi:
    enabled: {str(mroi).lower()}
    multiband_string: ''
sensor_trigger:
  enabled: true
reference:
  duration_s: 5.0
output:
  wavelengths:
    source: none
"""


class AcquisitionConfigSyncTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(TemporaryDirectory()))
        formal = self.root / "formal"
        snapshots = self.root / "snapshots"
        formal.mkdir()
        snapshots.mkdir()
        self.paths = {
            "gox": (formal / "config-gox.yaml", snapshots / "config-gox-snapshot.yaml"),
            "fx10": (formal / "config-fx10.yaml", snapshots / "config-fx10-snapshot.yaml"),
        }
        main = formal / "config-main.yaml"
        main.write_text("General: {}\n", encoding="utf-8")
        self.registry = {
            "main": ConfigFile("main", "Main", main),
            **{sensor: ConfigFile(sensor, sensor, paths[0]) for sensor, paths in self.paths.items()},
        }
        self.before = {}
        self.write_pair("gox", gox_document(), gox_document(reverse=True, mode="freerun"))
        self.write_pair("fx10", fx10_document(), fx10_document(mode="freerun"))
        self.state = SimpleNamespace(
            snapshot_busy=False, tool_uncertain=False, control_uncertain=False,
            config_locked=False, pending_config_notice=False, process_state=ProcState.IDLE,
        )
        self.stack.enter_context(patch.object(config_store, "STATE", self.state))
        self.stack.enter_context(patch.object(config_store, "REPO_ROOT", self.root))
        self.stack.enter_context(patch.object(config_store, "CONFIG_FILES", self.registry))
        self.stack.enter_context(patch.object(config_store, "SNAPSHOT_CONFIG", self.paths["gox"][1]))
        self.stack.enter_context(patch.object(config_store, "FX10_SNAPSHOT_CONFIG", self.paths["fx10"][1]))
        self.require_owner = self.stack.enter_context(patch.object(config_store.control_owner, "require"))
        self.stack.enter_context(patch.dict(os.environ, {"AMIGA_CONFIG_ROOT": str(self.root)}))

    def write_pair(self, sensor, formal, snapshot):
        for path, text in zip(self.paths[sensor], (formal, snapshot)):
            path.write_text(text, encoding="utf-8")
            self.before[path] = text.encode("utf-8")

    def apply(self, sensor):
        if sensor == "gox":
            return config_store.apply_gox_acquisition(TARGET_IP, TARGET_MAC, 25.0, 2.5)
        return config_store.apply_fx10_acquisition(8.0, 2, 4)

    def assert_unchanged(self, sensor, *, no_backups=True):
        for path in self.paths[sensor]:
            self.assertEqual(path.read_bytes(), self.before[path])
            if no_backups:
                self.assertFalse(path.with_suffix(".yaml.bak").exists())

    def test_gox_updates_same_camera_in_both_profiles_and_keeps_timing_comments(self):
        config_store.apply_gox_acquisition(TARGET_IP, TARGET_MAC, 25.0, 2.5)
        for path in self.paths["gox"]:
            with self.subTest(path=path.name):
                actual_text = path.read_text(encoding="utf-8")
                expected = yaml.safe_load(self.before[path])
                selected = next(camera for camera in expected["cameras"] if camera["id"] == "selected")
                selected["acquisition"].update(exposure_ms=25.0, gain=2.5)
                self.assertEqual(yaml.safe_load(actual_text), expected)
                for comment in ("# selected exposure comment", "# selected gain comment",
                                "# GOX timing and camera identity must survive Apply"):
                    self.assertIn(comment, actual_text)
                self.assertEqual(path.with_suffix(".yaml.bak").read_bytes(), self.before[path])

    def test_gox_exposure_only_apply_leaves_gain_untouched(self):
        summary = config_store.apply_gox_acquisition(TARGET_IP, TARGET_MAC, 25.0, None)
        self.assertIn("gain unchanged", summary)
        for path in self.paths["gox"]:
            with self.subTest(path=path.name):
                expected = yaml.safe_load(self.before[path])
                selected = next(camera for camera in expected["cameras"] if camera["id"] == "selected")
                selected["acquisition"]["exposure_ms"] = 25.0
                self.assertEqual(yaml.safe_load(path.read_text(encoding="utf-8")), expected)

    def test_fx10_updates_both_profiles_and_keeps_timing_comments_and_other_fields(self):
        config_store.apply_fx10_acquisition(8.0, 2, 4)
        for path in self.paths["fx10"]:
            with self.subTest(path=path.name):
                actual_text = path.read_text(encoding="utf-8")
                expected = yaml.safe_load(self.before[path])
                expected["acquisition"].update(exposure_ms=8.0, spatial_binning=2, spectral_binning=4)
                self.assertEqual(yaml.safe_load(actual_text), expected)
                for comment in ("# keep exposure documentation", "# keep spatial documentation",
                                "# keep spectral documentation"):
                    self.assertIn(comment, actual_text)
                self.assertEqual(path.with_suffix(".yaml.bak").read_bytes(), self.before[path])

    def test_fx10_exposure_only_apply_leaves_binning_untouched(self):
        summary = config_store.apply_fx10_acquisition(8.0, None, None)
        self.assertIn("binning unchanged", summary)
        for path in self.paths["fx10"]:
            with self.subTest(path=path.name):
                expected = yaml.safe_load(self.before[path])
                expected["acquisition"]["exposure_ms"] = 8.0
                self.assertEqual(yaml.safe_load(path.read_text(encoding="utf-8")), expected)
        with self.assertRaises(config_store.ApplyError):
            config_store.apply_fx10_acquisition(8.0, 2, None)

    def test_fx10_exposure_limit_is_the_strictest_frame_period(self):
        limit, notes = config_store.fx10_exposure_limit_ms()
        rates = [float(yaml.safe_load(self.before[path])["acquisition"].get("frame_rate_hz", 50.0))
                 for path in self.paths["fx10"]]
        self.assertAlmostEqual(limit, min(900.0 / rate for rate in rates))
        self.assertEqual(len(notes), 2)

    def test_gox_snapshot_enforces_freerun_margin_before_either_config_is_written(self):
        # 95 ms fits 1000/10 but violates the driver's required 900/10 margin.
        # Snapshot tools use freerun even if this YAML still requests external trigger.
        self.write_pair("gox", gox_document(mode="external"),
                        gox_document(reverse=True, mode="external", rate=10.0))
        with self.assertRaises(config_store.ApplyError):
            config_store.apply_gox_acquisition(TARGET_IP, TARGET_MAC, 95.0, 2.5)
        self.assert_unchanged("gox")

    def test_snapshot_camera_mac_mismatch_does_not_partially_update_formal_config(self):
        self.write_pair("gox", gox_document(), gox_document(reverse=True, mac="00:11:22:33:44:99"))
        with self.assertRaises(config_store.ApplyError):
            self.apply("gox")
        self.assert_unchanged("gox")

    def test_gox_snapshot_target_must_be_the_first_camera_before_either_write(self):
        # Formal recording can select any camera entry; jai_snapshot uses only cameras[0].
        self.write_pair("gox", gox_document(reverse=True), gox_document(mode="freerun"))
        with self.assertRaises(config_store.ApplyError):
            self.apply("gox")
        self.assert_unchanged("gox")

    def test_fx10_mroi_is_checked_in_each_profile_before_any_write(self):
        for formal_mroi, snapshot_mroi in ((True, False), (False, True)):
            with self.subTest(formal_mroi=formal_mroi, snapshot_mroi=snapshot_mroi):
                self.write_pair("fx10", fx10_document(mroi=formal_mroi),
                                fx10_document(mode="freerun", mroi=snapshot_mroi))
                with self.assertRaises(config_store.ApplyError):
                    self.apply("fx10")
                self.assert_unchanged("fx10")

    def test_fx10_exposure_margin_is_checked_in_both_profiles_including_default_rate(self):
        cases = ((100.0, 50.0, 10.0), (50.0, 100.0, 10.0), (10.0, None, 18.0))
        for formal_rate, snapshot_rate, exposure_ms in cases:
            with self.subTest(formal_rate=formal_rate, snapshot_rate=snapshot_rate,
                              exposure_ms=exposure_ms):
                # The C++ acquisition loader requires exposure < 900 / rate for
                # every trigger mode; an omitted rate defaults to 50 Hz.
                self.write_pair("fx10", fx10_document(mode="external", rate=formal_rate),
                                fx10_document(mode="external", rate=snapshot_rate))
                with self.assertRaises(config_store.ApplyError):
                    config_store.apply_fx10_acquisition(exposure_ms, 1, 1)
                self.assert_unchanged("fx10")

    def test_failed_second_replace_restores_first_config(self):
        original_replace = Path.replace
        for sensor in ("gox", "fx10"):
            with self.subTest(sensor=sensor):
                formal, snapshot = self.paths[sensor]
                replaced = []
                failed = False

                def fail_snapshot_once(source, target):
                    nonlocal failed
                    destination = Path(target)
                    if destination == snapshot and not failed:
                        failed = True
                        raise OSError("simulated snapshot replacement failure")
                    result = original_replace(source, target)
                    replaced.append(destination)
                    return result

                with patch.object(Path, "replace", new=fail_snapshot_once):
                    with self.assertRaises(config_store.ApplyError):
                        self.apply(sensor)
                self.assertTrue(failed)
                self.assertGreaterEqual(replaced.count(formal), 2)
                self.assert_unchanged(sensor, no_backups=False)

    def test_snapshot_cannot_collide_with_formal_backup_destination(self):
        for sensor, constant in (("gox", "SNAPSHOT_CONFIG"), ("fx10", "FX10_SNAPSHOT_CONFIG")):
            with self.subTest(sensor=sensor):
                formal, snapshot = self.paths[sensor]
                collision = formal.with_suffix(".yaml.bak")
                snapshot_bytes = snapshot.read_bytes()
                collision.write_bytes(snapshot_bytes)
                with patch.object(config_store, constant, collision):
                    with self.assertRaises(config_store.ApplyError):
                        self.apply(sensor)
                self.assertEqual(formal.read_bytes(), self.before[formal])
                self.assertEqual(collision.read_bytes(), snapshot_bytes)
                self.assertEqual(snapshot.read_bytes(), self.before[snapshot])

    def test_publishing_backups_replaces_symlinks_without_writing_their_targets(self):
        outside = Path(self.stack.enter_context(TemporaryDirectory()))
        sentinel = outside / "unrelated.txt"
        sentinel_bytes = b"unrelated file outside AMIGA_CONFIG_ROOT must stay intact\n"
        sentinel.write_bytes(sentinel_bytes)
        for sensor in ("gox", "fx10"):
            with self.subTest(sensor=sensor):
                for path in self.paths[sensor]:
                    path.with_suffix(".yaml.bak").symlink_to(sentinel)
                self.apply(sensor)
                for path in self.paths[sensor]:
                    backup = path.with_suffix(".yaml.bak")
                    self.assertFalse(backup.is_symlink())
                    self.assertEqual(backup.read_bytes(), self.before[path])
                self.assertEqual(sentinel.read_bytes(), sentinel_bytes)

    def test_configuration_locks_and_control_ownership_prevent_both_writes(self):
        for sensor in ("gox", "fx10"):
            for lock in ("snapshot_busy", "tool_uncertain", "control_uncertain", "config_locked"):
                with self.subTest(sensor=sensor, lock=lock):
                    setattr(self.state, lock, True)
                    try:
                        with self.assertRaises(config_store.ConfigBusyError):
                            self.apply(sensor)
                    finally:
                        setattr(self.state, lock, False)
                    self.assert_unchanged(sensor)
            self.require_owner.side_effect = RuntimeError("Not the control owner")
            with self.assertRaisesRegex(RuntimeError, "Not the control owner"):
                self.apply(sensor)
            self.assert_unchanged(sensor)
            self.require_owner.side_effect = None

    def test_snapshot_path_must_remain_inside_allowed_configuration_root(self):
        # The formal profiles stay allowed; only the snapshot profiles are outside.
        with patch.dict(os.environ, {"AMIGA_CONFIG_ROOT": str(self.root / "formal")}):
            for sensor in ("gox", "fx10"):
                with self.subTest(sensor=sensor), self.assertRaises(ValueError):
                    self.apply(sensor)
                self.assert_unchanged(sensor)
