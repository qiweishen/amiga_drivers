"""Server folder browser contracts; only temporary directories are inspected.

These tests do not start the GUI, launch a desktop dialog, or access devices.
"""

from contextlib import ExitStack
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from app.services import folder_picker


class _Scandir:
    """Small scandir stand-in for the bounded-listing cases."""

    def __init__(self, entries):
        self.entries = iter(entries)
        self.visited = 0

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def __iter__(self):
        return self

    def __next__(self):
        entry = next(self.entries)
        self.visited += 1
        return entry


class FolderBrowserTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.directory = Path(self.stack.enter_context(TemporaryDirectory())).resolve()
        self.root = self.directory / "recordings"
        self.root.mkdir()
        self.stack.enter_context(patch.dict(os.environ, {"AMIGA_DATA_ROOT": str(self.root)}))
        self.stack.enter_context(patch.object(folder_picker.config_store, "REPO_ROOT", self.root))

    def test_missing_initial_uses_nearest_existing_parent_without_creating_it(self):
        existing = self.root / "field"
        existing.mkdir()
        missing = existing / "future" / "session"
        self.assertEqual(folder_picker.initial_directory(str(missing)), existing)
        self.assertFalse(missing.parent.exists())

    def test_relative_initial_uses_project_root_and_blank_initial_uses_browser_root(self):
        data = self.root / "data"
        data.mkdir()
        self.assertEqual(folder_picker.initial_directory("./data"), data)
        self.assertEqual(folder_picker.initial_directory(""), self.root)

    def test_initial_rejects_control_characters_even_in_an_otherwise_blank_draft(self):
        for raw in ("\n", "\t", "\x85", " \n "):
            with self.subTest(raw=repr(raw)), self.assertRaisesRegex(RuntimeError, "control characters"):
                folder_picker.initial_directory(raw)

    def test_outside_initial_opens_browser_root(self):
        for value in (self.root.parent, self.root / "..", self.directory / "recordings-other"):
            with self.subTest(value=value):
                self.assertEqual(folder_picker.initial_directory(str(value)), self.root)

    def test_unset_data_root_is_project_root_not_filesystem_root(self):
        with patch.dict(os.environ, {"AMIGA_DATA_ROOT": ""}):
            listing = folder_picker.list_directory(str(self.root))
            self.assertEqual(listing.root, self.root)
            self.assertEqual(listing.path, self.root)
            self.assertIsNone(listing.parent)
            self.assertEqual(folder_picker.initial_directory(str(self.root.parent)), self.root)
            for operation in (folder_picker.list_directory, folder_picker.checked_selection):
                with self.subTest(operation=operation.__name__), self.assertRaises(RuntimeError):
                    operation(str(self.root.parent))

    def test_list_is_single_level_directories_only_and_sorted(self):
        names = ("zulu", "beta", "alpha", "Alpha")
        for name in names:
            (self.root / name).mkdir()
        (self.root / "alpha" / "nested").mkdir()
        (self.root / "notes.txt").write_text("not read by the browser", encoding="utf-8")
        with patch.object(folder_picker.os, "scandir", wraps=os.scandir) as scan, \
                patch.object(Path, "open", side_effect=AssertionError("must not read file contents")), \
                patch.object(Path, "mkdir", side_effect=AssertionError("must not create directories")):
            listing = folder_picker.list_directory(str(self.root))
        self.assertEqual(listing.root, self.root)
        self.assertEqual(listing.path, self.root)
        self.assertIsNone(listing.parent)
        self.assertEqual(listing.directories, tuple(self.root / name for name in ("Alpha", "alpha", "beta", "zulu")))
        self.assertFalse(listing.truncated)
        self.assertEqual(scan.call_count, 1)
        self.assertEqual(Path(scan.call_args.args[0]), self.root)

    def test_child_listing_has_parent_inside_root(self):
        child = self.root / "field"
        child.mkdir()
        listing = folder_picker.list_directory(str(child))
        self.assertEqual(listing.parent, self.root)
        self.assertEqual(listing.directories, ())
        self.assertFalse(listing.truncated)

    def test_selection_and_listing_reject_relative_missing_file_and_outside_paths(self):
        regular_file = self.root / "notes.txt"
        regular_file.write_text("not a directory", encoding="utf-8")
        sibling = self.directory / "recordings-other"
        sibling.mkdir()
        values = ("relative/path", str(self.root / "missing"), str(regular_file),
                  str(self.root.parent), str(self.root / ".."), str(sibling))
        for operation in (folder_picker.list_directory, folder_picker.checked_selection):
            for value in values:
                with self.subTest(operation=operation.__name__, value=value), self.assertRaises(RuntimeError):
                    operation(value)

    def test_missing_or_non_directory_root_is_not_replaced_by_a_broader_root(self):
        regular_file = self.directory / "file-root"
        regular_file.write_text("not a directory", encoding="utf-8")
        for root in (self.directory / "missing-root", regular_file):
            with patch.dict(os.environ, {"AMIGA_DATA_ROOT": str(root)}):
                for operation in (folder_picker.initial_directory, folder_picker.list_directory,
                                  folder_picker.checked_selection):
                    with self.subTest(root=root, operation=operation.__name__), self.assertRaises(RuntimeError):
                        operation(str(root))

    def test_hidden_directories_are_not_listed_or_directly_entered(self):
        hidden = self.root / ".private"
        hidden.mkdir()
        nested = hidden / "visible-name"
        nested.mkdir()
        self.assertEqual(folder_picker.list_directory(str(self.root)).directories, ())
        for path in (hidden, nested):
            for operation in (folder_picker.list_directory, folder_picker.checked_selection):
                with self.subTest(path=path, operation=operation.__name__), self.assertRaises(RuntimeError):
                    operation(str(path))

    def test_hidden_initial_uses_nearest_visible_parent(self):
        visible = self.root / "field"
        visible.mkdir()
        hidden = visible / ".cache"
        hidden.mkdir()
        nested = hidden / "session"
        nested.mkdir()
        for path in (hidden, nested, nested / "future"):
            with self.subTest(path=path):
                self.assertEqual(folder_picker.initial_directory(str(path)), visible)

    def test_hidden_root_and_its_ancestors_do_not_hide_visible_children(self):
        hidden_parent = self.directory / ".private-parent"
        hidden_root = hidden_parent / ".recordings"
        child = hidden_root / "field"
        child.mkdir(parents=True)
        with patch.dict(os.environ, {"AMIGA_DATA_ROOT": str(hidden_root)}):
            listing = folder_picker.list_directory(str(hidden_root))
            self.assertEqual(listing.root, hidden_root)
            self.assertEqual(listing.directories, (child,))
            self.assertIsNone(listing.parent)
            self.assertEqual(folder_picker.checked_selection(str(child)), str(child))
            self.assertEqual(folder_picker.initial_directory(str(child)), child)

    def test_external_symlink_is_filtered_and_cannot_be_selected_or_entered(self):
        outside = self.root / "outside"
        outside.symlink_to(self.directory, target_is_directory=True)
        self.assertEqual(folder_picker.list_directory(str(self.root)).directories, ())
        self.assertEqual(folder_picker.initial_directory(str(outside)), self.root)
        for operation in (folder_picker.list_directory, folder_picker.checked_selection):
            with self.subTest(operation=operation.__name__), self.assertRaises(RuntimeError):
                operation(str(outside))

    def test_visible_internal_symlink_returns_canonical_paths(self):
        target = self.root / "field"
        target.mkdir()
        alias = self.root / "linked-field"
        alias.symlink_to(target, target_is_directory=True)
        self.assertEqual(folder_picker.checked_selection(str(alias)), str(target))
        self.assertEqual(folder_picker.initial_directory(str(alias)), target)
        listing = folder_picker.list_directory(str(alias))
        self.assertEqual(listing.path, target)
        self.assertEqual(listing.parent, self.root)
        children = folder_picker.list_directory(str(self.root)).directories
        self.assertIn(target, children)
        self.assertTrue(all(path == target for path in children))

    def test_symlink_to_hidden_target_and_hidden_alias_cannot_bypass_visibility(self):
        hidden_target = self.root / ".private"
        hidden_target.mkdir()
        visible_alias = self.root / "visible-alias"
        visible_alias.symlink_to(hidden_target, target_is_directory=True)
        visible_target = self.root / "field"
        visible_target.mkdir()
        hidden_alias = self.root / ".hidden-alias"
        hidden_alias.symlink_to(visible_target, target_is_directory=True)
        self.assertEqual(folder_picker.list_directory(str(self.root)).directories, (visible_target,))
        for path in (visible_alias, hidden_alias):
            for operation in (folder_picker.list_directory, folder_picker.checked_selection):
                with self.subTest(path=path, operation=operation.__name__), self.assertRaises(RuntimeError):
                    operation(str(path))

    def test_control_characters_are_rejected_before_and_after_resolving(self):
        for index, character in enumerate(("\n", "\x7f", "\x85")):
            target = self.root / f"bad{character}name"
            target.mkdir()
            alias = self.root / f"clean-name-{index}"
            alias.symlink_to(target, target_is_directory=True)
            for path in (target, alias):
                for operation in (folder_picker.list_directory, folder_picker.checked_selection):
                    with self.subTest(path=path, operation=operation.__name__), self.assertRaises(RuntimeError):
                        operation(str(path))
        self.assertEqual(folder_picker.list_directory(str(self.root)).directories, ())

    def test_missing_and_looping_symlinks_are_filtered_and_direct_access_reports_errors(self):
        missing = self.root / "missing-link"
        missing.symlink_to(self.root / "absent", target_is_directory=True)
        first = self.root / "loop-a"
        second = self.root / "loop-b"
        first.symlink_to(second, target_is_directory=True)
        second.symlink_to(first, target_is_directory=True)
        self.assertEqual(folder_picker.list_directory(str(self.root)).directories, ())
        for path in (missing, first):
            for operation in (folder_picker.list_directory, folder_picker.checked_selection):
                with self.subTest(path=path, operation=operation.__name__), self.assertRaises(RuntimeError) as raised:
                    operation(str(path))
                self.assertTrue(str(raised.exception))

    def test_current_directory_permission_or_disappearance_is_reported(self):
        for error in (PermissionError("permission denied"), FileNotFoundError("directory disappeared")):
            with self.subTest(error=type(error).__name__), \
                    patch.object(folder_picker.os, "scandir", side_effect=error), \
                    self.assertRaises(RuntimeError) as raised:
                folder_picker.list_directory(str(self.root))
            self.assertTrue(str(raised.exception))

    def test_selection_reports_permission_or_disappearance_after_navigation(self):
        folder_picker.list_directory(str(self.root))
        for error in (PermissionError("permission denied"), FileNotFoundError("directory disappeared")):
            with self.subTest(error=type(error).__name__):
                with patch.object(folder_picker.os, "scandir", side_effect=error), \
                        self.assertRaises(RuntimeError) as raised:
                    folder_picker.checked_selection(str(self.root))
                self.assertTrue(str(raised.exception))

    def test_selection_reuses_shared_output_directory_policy(self):
        child = self.root / "field"
        child.mkdir()
        with patch.object(folder_picker.config_store, "output_dir_problems", return_value=[]) as validate:
            self.assertEqual(folder_picker.checked_selection(str(child)), str(child))
        validate.assert_called_once_with(str(child))
        with patch.object(folder_picker.config_store, "output_dir_problems", return_value=["recording mount unavailable"]):
            with self.assertRaisesRegex(RuntimeError, "recording mount unavailable"):
                folder_picker.checked_selection(str(child))

    def test_listing_limit_is_bounded_sorted_and_reports_truncation(self):
        for count, truncated in ((500, False), (600, True)):
            entries = [SimpleNamespace(name=f"field-{index:04d}",
                                       path=str(self.root / f"field-{index:04d}"),
                                       is_dir=Mock(return_value=True))
                       for index in range(count, 0, -1)]
            scan = _Scandir(entries)
            with self.subTest(count=count), \
                    patch.object(folder_picker.os, "scandir", return_value=scan), \
                    patch.object(Path, "resolve", autospec=True, side_effect=lambda path, *args, **kwargs: path), \
                    patch.object(Path, "is_dir", return_value=True):
                listing = folder_picker.list_directory(str(self.root))
            self.assertEqual(len(listing.directories), 500)
            self.assertEqual(listing.truncated, truncated)
            self.assertLessEqual(scan.visited, 501)
            self.assertEqual(listing.directories,
                             tuple(sorted(listing.directories, key=lambda path: (path.name.casefold(), path.name))))
            self.assertTrue(all(path.parent == self.root for path in listing.directories))
