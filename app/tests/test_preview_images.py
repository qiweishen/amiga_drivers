"""In-memory preview image store contracts; no web server or camera."""

import threading
import unittest
from unittest.mock import patch

from app.services import preview_images


def _key(url: str) -> str:
    return url.removeprefix(f"{preview_images.ROUTE}/")


class PreviewImageStoreTests(unittest.TestCase):
    def setUp(self):
        preview_images.clear()

    def tearDown(self):
        preview_images.clear()

    def test_ready_bytes_are_served_with_a_safe_filename(self):
        url = preview_images.put(b"png-bytes", 'ref "x"/../y.png')
        self.assertTrue(url.startswith(f"{preview_images.ROUTE}/"))
        data, media_type, filename = preview_images.fetch(_key(url))
        self.assertEqual((data, media_type), (b"png-bytes", "image/png"))
        self.assertNotIn("/", filename)
        self.assertNotIn('"', filename)
        self.assertEqual(preview_images.download_url(url), f"{url}?download=1")

    def test_lazy_producer_runs_once_even_under_concurrent_requests(self):
        calls = []
        gate = threading.Event()

        def produce():
            calls.append(1)
            gate.wait(1.0)
            return b"full"

        key = _key(preview_images.lazy(produce, "full.png"))
        threads = [threading.Thread(target=preview_images.fetch, args=(key,)) for _ in range(4)]
        for thread in threads:
            thread.start()
        gate.set()
        for thread in threads:
            thread.join()
        self.assertEqual(len(calls), 1)
        self.assertEqual(preview_images.fetch(key)[0], b"full")

    def test_failed_producer_is_reported_and_can_be_retried(self):
        outcomes = [OSError("raw deleted"), b"ok"]

        def produce():
            outcome = outcomes.pop(0)
            if isinstance(outcome, Exception):
                raise outcome
            return outcome

        key = _key(preview_images.lazy(produce, "full.png"))
        with self.assertRaisesRegex(OSError, "raw deleted"):
            preview_images.fetch(key)
        self.assertEqual(preview_images.fetch(key)[0], b"ok")

    def test_release_and_unknown_keys(self):
        url = preview_images.put(b"x", "x.png")
        preview_images.release(url, None, "/elsewhere/abc")
        self.assertIsNone(preview_images.fetch(_key(url)))
        self.assertIsNone(preview_images.fetch("missing"))
        with self.assertRaises(ValueError):
            preview_images.put(b"", "empty.png")

    def test_least_recently_used_entries_are_evicted_by_count_and_bytes(self):
        with patch.object(preview_images, "MAX_ENTRIES", 2):
            first = _key(preview_images.put(b"1", "1.png"))
            second = _key(preview_images.put(b"2", "2.png"))
            preview_images.fetch(first)  # first becomes most recently used
            preview_images.put(b"3", "3.png")
            self.assertIsNotNone(preview_images.fetch(first))
            self.assertIsNone(preview_images.fetch(second))
        preview_images.clear()
        with patch.object(preview_images, "MAX_BYTES", 10):
            old = _key(preview_images.put(b"123456", "old.png"))
            new = _key(preview_images.put(b"abcdef", "new.png"))
            self.assertIsNone(preview_images.fetch(old))
            self.assertEqual(preview_images.fetch(new)[0], b"abcdef")


if __name__ == "__main__":
    unittest.main()
