"""FX10 raw-only recording and native-DN statistics contracts; no device access."""

import copy
import json
import numpy as np
import tempfile
import unittest
from pathlib import Path

from app.decoder import fx10
from app.decoder.fx10 import recording as fx10_recording
from app.tests.test_ebus_raw_format import fixture_header, fixture_record, write_fixture


class Fx10FormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.camera = self.root / "camera"
        self.payload, self.meta = write_fixture(self.camera)
        self.segment = self.camera / "seg_00001.raw"
        self.expected = np.array([[[0xABC, 0x123], [0xFFF, 0]]], dtype=np.uint16)

    def test_snapshot_live_and_reference_use_the_same_raw_dn(self):
        before = self.segment.read_bytes()
        capture, cube = fx10.snapshot_cube(self.camera)
        tail, host_timestamp = fx10.raw_tail_cube(self.camera, capture, 50)
        _, count, values, mean, clipped = fx10.reference_spectrum(self.camera, 1)
        np.testing.assert_array_equal(cube, self.expected)
        np.testing.assert_array_equal(tail, self.expected)
        self.assertEqual(host_timestamp, self.meta["host_realtime_ns"])
        self.assertEqual(count, 1)  # the retained INCOMPLETE record is not a frame
        direct = fx10.spectrum(cube, capture.full_scale)
        for name in values:
            np.testing.assert_allclose(values[name], direct[0][name])
        self.assertAlmostEqual(mean, direct[1])
        self.assertAlmostEqual(clipped, direct[2])
        self.assertEqual(values["max"], [2748.0, 4095.0])
        self.assertEqual(values["mean"], [1519.5, 2047.5])
        self.assertEqual(mean, 1783.5)
        self.assertEqual(clipped, 25.0)
        self.assertEqual(self.segment.read_bytes(), before)

    def test_dn_reductions_preserve_out_of_range_values_and_fractional_statistics(self):
        cube = np.array([[[0, 4095], [10, 20]], [[1, 2], [3, 4]],
                         [[4096, 30], [40, 65535]]], dtype=np.uint16)
        original = cube.copy()
        expected_values, expected_mean, expected_clipped = fx10.spectrum(cube, 4095)
        histogram = fx10.BandHistograms(2, 4095)
        histogram.add(cube[:1])
        histogram.add(cube[1:])
        values, mean, clipped = histogram.finish()
        for name in values:
            np.testing.assert_allclose(values[name], expected_values[name])
        self.assertAlmostEqual(mean, expected_mean)
        self.assertAlmostEqual(clipped, expected_clipped)
        self.assertEqual(values["max"], [4096.0, 65535.0])
        self.assertAlmostEqual(values["mean"][0], 8224.0 / 6)
        self.assertEqual(clipped, 25.0)
        np.testing.assert_array_equal(cube, original)

    def test_profile_types_pixel_format_and_wavelengths_are_not_guessed(self):
        path = self.camera / "device.json"
        original = json.loads(path.read_text())
        for key, value in (("samples", True), ("samples", 2.5), ("bands", 0),
                           ("full_scale", 1023), ("pixel_format", "RGB8"),
                           ("pixel_format", []), ("wavelengths_nm", [500.0]),
                           ("wavelengths_nm", [500.0, True]),
                           ("expected_frame_rate_hz", float("nan"))):
            with self.subTest(key=key, value=value):
                document = copy.deepcopy(original)
                document["profile"][key] = value
                path.write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    fx10.load_capture(self.camera)

    def test_raw_image_geometry_must_match_the_recorded_profile(self):
        path = self.camera / "device.json"
        document = json.loads(path.read_text())
        document["profile"]["samples"] = 3
        path.write_text(json.dumps(document))
        capture = fx10.load_capture(self.camera)
        with self.assertRaises(ValueError):
            fx10.snapshot_cube(self.camera)
        with self.assertRaises(ValueError):
            fx10.raw_tail_cube(self.camera, capture, 1)

    def test_legacy_envi_directory_is_explicitly_rejected(self):
        directory = self.root / "legacy"
        directory.mkdir()
        (directory / "capture.json").write_text(json.dumps({"format": "fx10-capture-v1"}))
        (directory / "segment_0001.hdr").write_text("ENVI\n")
        (directory / "segment_0001.bil").write_bytes(self.expected.tobytes())
        for read in (lambda: fx10.load_capture(directory),
                     lambda: fx10.snapshot_cube(directory),
                     lambda: fx10.validate_finalized(directory),
                     lambda: fx10.reference_spectrum(directory, 1)):
            with self.assertRaisesRegex(ValueError, "only ebus-raw"):
                read()

    def test_gox_raw_profile_is_rejected_as_fx10(self):
        directory = self.root / "gox"
        write_fixture(directory, kind="gox")
        with self.assertRaisesRegex(ValueError, "not an FX10"):
            fx10.load_capture(directory)

    def test_live_tail_ignores_torn_record_but_finalized_readers_reject_it(self):
        capture = fx10.load_capture(self.camera)
        complete = fixture_header() + fixture_record(self.meta, self.payload)
        self.segment.write_bytes(complete + fixture_record(self.meta, self.payload, 1)[:-3])
        cube, _ = fx10.raw_tail_cube(self.camera, capture, 10)
        np.testing.assert_array_equal(cube, self.expected)
        for read in (lambda: fx10.snapshot_cube(self.camera),
                     lambda: fx10.validate_finalized(self.camera),
                     lambda: fx10.reference_spectrum(self.camera, 1)):
            with self.assertRaises(ValueError):
                read()

    def test_finalization_and_reference_counts_must_match_raw_records(self):
        fx10.validate_finalized(self.camera)
        with self.assertRaises(ValueError):
            fx10.reference_spectrum(self.camera, 2)
        events = self.camera / "events.jsonl"
        document = json.loads(events.read_text())
        document["records_written"] += 1
        events.write_text(json.dumps(document) + "\n")
        with self.assertRaises(ValueError):
            fx10.validate_finalized(self.camera)
        with self.assertRaises(ValueError):
            fx10.snapshot_cube(self.camera)

    def test_decoded_batches_are_independent_of_chunk_and_padding_bytes(self):
        capture = fx10.load_capture(self.camera)
        batches = list(fx10_recording.raw_frame_batches(self.camera, capture, batch_bytes=1))
        self.assertEqual(len(batches), 1)
        np.testing.assert_array_equal(batches[0], self.expected)
        self.assertEqual(batches[0].nbytes, capture.line_bytes)


if __name__ == "__main__":
    unittest.main()
