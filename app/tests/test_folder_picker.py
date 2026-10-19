"""Native folder selection contracts; subprocesses are mocked."""

import asyncio
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, Mock, patch

from app.services import folder_picker


class FolderPathTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.environ = patch.dict(os.environ, {"AMIGA_DATA_ROOT": str(self.root)})
        self.environ.start()
        self.addCleanup(self.environ.stop)

    def test_missing_initial_uses_existing_parent_without_creating_it(self):
        missing = self.root / "future" / "session"
        self.assertEqual(folder_picker._initial_directory(str(missing)), self.root)
        self.assertFalse(missing.parent.exists())

    def test_outside_initial_opens_shared_root(self):
        self.assertEqual(folder_picker._initial_directory(str(self.root.parent)), self.root)

    def test_selection_must_be_existing_and_inside_root(self):
        self.assertEqual(folder_picker._checked_selection(str(self.root)), str(self.root))
        for value in (str(self.root.parent), str(self.root / "missing"), "relative/path"):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                folder_picker._checked_selection(value)

    def test_symlink_cannot_escape_root_or_hide_control_characters(self):
        outside = self.root / "outside"
        outside.symlink_to(self.root.parent, target_is_directory=True)
        with self.assertRaises(RuntimeError):
            folder_picker._checked_selection(str(outside))
        target = self.root / "bad\nname"
        target.mkdir()
        alias = self.root / "clean-name"
        alias.symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, "control characters"):
            folder_picker._checked_selection(str(alias))


class FolderDialogTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.patches = [
            patch.dict(os.environ, {"DISPLAY": ":0"}),
            patch.object(folder_picker.shutil, "which", return_value="/usr/bin/zenity"),
            patch.object(folder_picker, "_initial_directory", return_value=Path("/recordings")),
            patch.object(folder_picker, "_active", False),
        ]
        for item in self.patches:
            item.start()
            self.addCleanup(item.stop)

    async def test_cancel_returns_none_and_releases_dialog(self):
        process = Mock(returncode=1, communicate=AsyncMock(return_value=(b"", b"")))
        with patch.object(folder_picker.asyncio, "create_subprocess_exec", AsyncMock(return_value=process)) as spawn:
            self.assertIsNone(await folder_picker.select_directory("./data"))
        self.assertFalse(folder_picker._active)
        process.kill.assert_not_called()
        self.assertIn("--directory", spawn.call_args.args)
        self.assertIn("--filename=/recordings/", spawn.call_args.args)

    async def test_missing_zenity_reports_unavailable_without_spawning(self):
        with patch.object(folder_picker.shutil, "which", return_value=None), \
                patch.object(folder_picker.asyncio, "create_subprocess_exec", AsyncMock()) as spawn:
            with self.assertRaisesRegex(RuntimeError, "zenity is not installed"):
                await folder_picker.select_directory("./data")
        spawn.assert_not_called()

    async def test_cancelled_request_kills_child_and_releases_single_dialog(self):
        started = asyncio.Event()
        calls = 0

        async def communicate():
            nonlocal calls
            calls += 1
            if calls == 1:
                started.set()
                await asyncio.Future()
            return b"", b""

        process = Mock(returncode=None, communicate=AsyncMock(side_effect=communicate))
        process.kill.side_effect = lambda: setattr(process, "returncode", -9)
        with patch.object(folder_picker.asyncio, "create_subprocess_exec", AsyncMock(return_value=process)):
            task = asyncio.create_task(folder_picker.select_directory("./data"))
            await started.wait()
            with self.assertRaisesRegex(RuntimeError, "already open"):
                await folder_picker.select_directory("./data")
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
        process.kill.assert_called_once()
        self.assertEqual(process.communicate.await_count, 2)
        self.assertFalse(folder_picker._active)
